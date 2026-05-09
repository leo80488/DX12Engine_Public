// AutoExposure.cs.hlsl
// Two compute shaders:
//   CSHistogramBuild   — accumulates a 256-bin log-luminance histogram
//   CSHistogramAverage — computes average log-luma and writes adapted exposure
//
// Root signature (space2):
//   [0] cbuffer PerDispatch b0 space2
//   [1] SRV t0 space2  — HDR scene (HistogramBuild input)
//   [4] UAV u0 space2  — histogram buffer  (RWByteAddressBuffer / RWStructuredBuffer<uint>)
//   [5] UAV u1 space2  — exposure buffer   (RWStructuredBuffer<float>)

cbuffer PerDispatch : register(b0, space2)
{
    uint  g_width;
    uint  g_height;
    float g_minLogLuma;     // log2 luma floor (e.g. -8)
    float g_invLogLumaRange;// 1 / (maxLogLuma - minLogLuma)
    float g_adaptationRate; // per-frame lerp factor (e.g. 0.05)
    float g_lowPercent;     // lower percentile to clip (e.g. 0.5)
    float g_highPercent;    // upper percentile to clip (e.g. 0.95)
    float g_minExposure;    // minimum exposure floor (e.g. 0.1)
    float g_maxExposure;    // maximum exposure ceiling
    float g_evBias;         // exposure compensation in EV stops (+1 = 2× brighter)
    float g_keyValue;       // target middle-grey key (default 0.18)
    float2 _pad;
}

Texture2D<float4>         g_hdr       : register(t0, space2);
RWStructuredBuffer<uint>  g_histogram : register(u0, space2);
RWStructuredBuffer<float> g_exposure  : register(u1, space2);

SamplerState g_linear : register(s0, space2);

static const uint HISTOGRAM_BINS = 256;

// BT.709 luminance
float Luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

uint LumaToBin(float luma)
{
    if (luma < 1e-5f) return 0;
    float logL = (log2(luma) - g_minLogLuma) * g_invLogLumaRange;
    return (uint)(saturate(logL) * float(HISTOGRAM_BINS - 1) + 0.5f);
}

// ------------------------------------------------------------------
// CSHistogramBuild: one thread per 8x8 tile, uses groupshared atomics
// ------------------------------------------------------------------
groupshared uint gs_histogram[HISTOGRAM_BINS];

[numthreads(16, 16, 1)]
void CSHistogramBuild(uint3 groupID : SV_GroupID,
                      uint3 localID : SV_GroupThreadID,
                      uint3 id      : SV_DispatchThreadID)
{
    // Clear groupshared histogram
    uint flatLocal = localID.y * 16 + localID.x;
    if (flatLocal < HISTOGRAM_BINS)
        gs_histogram[flatLocal] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Accumulate pixels in this tile
    if (id.x < g_width && id.y < g_height)
    {
        float4 hdrSample = g_hdr.Load(int3(id.xy, 0));
        float  luma      = Luma(hdrSample.rgb);
        uint   bin       = LumaToBin(luma);
        InterlockedAdd(gs_histogram[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // Write groupshared → global (one value per thread if flatLocal < BINS)
    if (flatLocal < HISTOGRAM_BINS)
        InterlockedAdd(g_histogram[flatLocal], gs_histogram[flatLocal]);
}

// ------------------------------------------------------------------
// CSHistogramAverage: single group, computes weighted average
// ------------------------------------------------------------------
groupshared uint  gs_hist[HISTOGRAM_BINS];
groupshared float gs_accumLuma;
groupshared uint  gs_totalPixels;

[numthreads(HISTOGRAM_BINS, 1, 1)]
void CSHistogramAverage(uint3 id : SV_DispatchThreadID)
{
    uint bin   = id.x;
    uint count = g_histogram[bin];

    // Zero out histogram for next frame
    g_histogram[bin] = 0;

    gs_hist[bin] = count;
    GroupMemoryBarrierWithGroupSync();

    // Single-thread reduction
    if (bin == 0)
    {
        uint  totalPixels = g_width * g_height;
        uint  lowCut  = (uint)(g_lowPercent  * float(totalPixels));
        uint  highCut = (uint)(g_highPercent * float(totalPixels));

        uint  cumul     = 0;
        float sumLogL   = 0.0f;
        uint  countUsed = 0;

        for (uint i = 0; i < HISTOGRAM_BINS; ++i)
        {
            uint n = gs_hist[i];
            uint newCumul = cumul + n;

            // Compute overlap with [lowCut, highCut]
            uint lo = max(cumul, lowCut);
            uint hi = min(newCumul, highCut);
            if (hi > lo)
            {
                uint used = hi - lo;
                float logL = g_minLogLuma + (float(i) + 0.5f) / float(HISTOGRAM_BINS - 1)
                             / g_invLogLumaRange;
                sumLogL   += logL * float(used);
                countUsed += used;
            }
            cumul = newCumul;
        }

        float targetLogLuma = (countUsed > 0) ? (sumLogL / float(countUsed)) : g_minLogLuma;
        float targetLuma    = exp2(targetLogLuma);

        // Exposure = key / average_luma × 2^EV_bias.
        // g_keyValue (default 0.18) sets the "middle-grey" target;
        // g_evBias lets artists compensate bright-sky IBL scenes (e.g. +1 EV).
        float key = max(g_keyValue, 1e-4f);
        float targetExposure = (targetLuma > 1e-5f) ? (key / targetLuma) : 1.0f;
        targetExposure *= exp2(g_evBias);

        // Temporal adaptation
        float prevExposure = g_exposure[0];
        if (isnan(prevExposure) || isinf(prevExposure) || prevExposure < 1e-6f) prevExposure = 1.0f;  // bootstrap
        float adaptedExposure = lerp(prevExposure, targetExposure, g_adaptationRate);
        if (isnan(adaptedExposure) || isinf(adaptedExposure)) adaptedExposure = targetExposure;
        g_exposure[0] = clamp(adaptedExposure, g_minExposure, g_maxExposure);
        
    }
}
