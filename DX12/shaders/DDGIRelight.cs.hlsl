// DDGIRelight.cs.hlsl — atlas/probe update from per-frame ray data.
//
// Two define-paths share this kernel:
//   DDGI_RELIGHT_TARGET_IRRADIANCE — write per-probe L1 SH into the SH buffer.
//   DDGI_RELIGHT_TARGET_DEPTH       — write the depth atlas tile (16×16+border).
//
// Irradiance path (after the SH refactor):
//   1. Each thread (lid.x, lid.y) of the 6×6 grid integrates the rays for its
//      octahedral direction with a cosine weight, then runs the multi-scale
//      mean estimator (firefly suppression + adaptive blend).
//   2. Mean is written to groupshared.
//   3. Group-thread-0 sums across the grid into L1 SH and stores into
//      g_ProbeSH[probeIdx]. Sample-time DDGI_SH_Irradiance(N) reconstructs
//      irradiance at a normal direction.
//
// Depth path: unchanged from the original (per-texel cosine-weighted integral
// of distance + distance², EMA blend with hysteresis).

#include "DDGICommon.hlsli"

#ifndef DDGI_RELIGHT_TARGET_IRRADIANCE
#define DDGI_RELIGHT_TARGET_IRRADIANCE 1
#endif
#ifndef DDGI_RELIGHT_TARGET_DEPTH
#define DDGI_RELIGHT_TARGET_DEPTH 0
#endif

ConstantBuffer<DDGIVolumeGPU>      g_Vol      : register(b0, space0);
// CRITICAL: g_RayData MUST live at t2 (not t0). The DDGIPass root signature
// uses t0 for the TLAS (ROOT_SRV slot 1), and binds the ray data SRV via
// the slot-4 descriptor table at register t2. Reading from t0 here would
// reinterpret the TLAS's internal driver bits as a float4 RWStructuredBuffer
// and write garbage (~1e30 magnitude) into probe[0]'s SH — symptom matches
// the magenta/saturated probes the engineer was investigating.
StructuredBuffer<float4>           g_RayData  : register(t2, space0);

#if DDGI_RELIGHT_TARGET_IRRADIANCE
RWStructuredBuffer<DDGIProbeSH>      g_ProbeSH  : register(u0, space0);
RWStructuredBuffer<DDGIVarianceData> g_Variance : register(u1, space0);
static const uint kProbeSize   = DDGI_IRRADIANCE_PROBE_SIZE;
static const uint kProbeStride = DDGI_IRRADIANCE_PROBE_STRIDE;
groupshared float3 sharedTexels[DDGI_IRRADIANCE_PROBE_SIZE * DDGI_IRRADIANCE_PROBE_SIZE];
#else
RWTexture2D<float2>                g_Atlas    : register(u0, space0);
static const uint kProbeSize   = DDGI_DEPTH_PROBE_SIZE;
static const uint kProbeStride = DDGI_DEPTH_PROBE_STRIDE;
#endif

// Adaptive per-probe ray count — written by DDGIPrepareRayCount.cs each frame.
RWStructuredBuffer<uint>           g_RayCount : register(u2, space0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    const uint probeIdx = tid.x;
    if (probeIdx >= g_Vol.probeCountsX * g_Vol.probeCountsY * g_Vol.probeCountsZ)
        return;

    const uint2 inProbeTexel = lid.xy;
    const bool  validTexel   = (inProbeTexel.x < kProbeSize && inProbeTexel.y < kProbeSize);

    // Adaptive ray count: this probe contributed only `adaptiveRays` ray
    // samples this frame. Direction generation now uses Halton(2,3), which
    // is prefix-uniform — `r` maps to the same direction regardless of how
    // many rays this frame's probe traced. Matches the trace CS. Ray data
    // is laid out at the volume's MAX raysPerProbe stride (buffer size).
    const uint adaptiveRays = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;
    const uint rayBase      = probeIdx * g_Vol.raysPerProbe;

    // ---- Per-texel ray integration ----------------------------------------
    float3 texelMean = float3(0, 0, 0);
    if (validTexel)
    {
        float3 texelDir = DDGI_DirectionFromTexel(inProbeTexel, kProbeSize);

#if DDGI_RELIGHT_TARGET_IRRADIANCE
        float3 sumColor = 0;
#else
        float  sumDist  = 0;
        float  sumDist2 = 0;
#endif
        float  sumWeight = 0;

        [loop] for (uint r = 0; r < adaptiveRays; ++r)
        {
            const float4 sample = g_RayData[rayBase + r];

            // Direction reconstruction matches trace CS exactly: same adaptive
            // sample index → same Halton point → same rotated rayDir.
            const uint sampleIdx = DDGI_AdaptiveSampleIdx(r, adaptiveRays, g_Vol);
            float3 dirLocal = DDGI_HaltonSphere(sampleIdx);
            float3 rayDir   = DDGI_ApplyRandomRotation(dirLocal, g_Vol);

            float w = saturate(dot(rayDir, texelDir));
#if DDGI_RELIGHT_TARGET_DEPTH
            w = pow(w, 64.0);
#endif
            if (w < 1e-3) continue;

            sumWeight += w;
#if DDGI_RELIGHT_TARGET_IRRADIANCE
            sumColor += sample.rgb * w;
#else
            float d = sample.w >= 0.0 ? sample.w : 100.0;
            sumDist  += d  * w;
            sumDist2 += d  * d * w;
#endif
        }

        const bool overwrite = (g_Vol.flags & 8u) != 0;

#if DDGI_RELIGHT_TARGET_IRRADIANCE
        float3 newSample = (sumWeight > 1e-4) ? (sumColor / sumWeight) : float3(0, 0, 0);
        const uint vidx = probeIdx * kProbeSize * kProbeSize
                        + inProbeTexel.y * kProbeSize + inProbeTexel.x;
        DDGIVarianceData vd = g_Variance[vidx];

        // Plain EMA on mean (deliberately not WickedEngine MultiscaleMeanEstimator
        // — its firefly clip + catchUpBlend collapsed to 1/256 per frame for
        // "converged" texels, so any low seed got locked in for ~250 frames at
        // this engine's 64-rays-per-probe budget). Plain EMA + trace shader's
        // `min(radiance, 5.0)` firefly cap is robust enough for diffuse-only DDGI.
        //
        // Inconsistency: per-texel "how much did this frame's sample diverge
        // from the running mean", smoothed across frames with the same EMA
        // speed. PrepareRayCount.cs aggregates the per-probe max and uses
        // that to decide each probe's per-frame ray budget — high
        // inconsistency keeps the probe at full budget, low collapses to
        // 1/4. Without this signal, every probe permanently drops to min
        // budget after the burn-in window (incRamp = 0).
        //
        // Range: instInc ∈ [0, 1]. PrepareRayCount maps [0.2, 1.0] linearly
        // to [minBuckets, maxBuckets], so anything below 0.2 is treated as
        // "fully converged" and gets minimal rays.
        const float blendSpeed = max(saturate(1.0 - g_Vol.hysteresis), 0.005);
        if (any(isnan(vd.mean)) || any(isinf(vd.mean))) vd.mean = float3(0, 0, 0);
        if (isnan(vd.inconsistency) || isinf(vd.inconsistency)) vd.inconsistency = 0.0;
        const bool overwriteFrame = (g_Vol.flags & 8u) != 0;

        // Compute inconsistency BEFORE updating mean (so we measure delta
        // against the current running mean, not the just-blended one).
        const float meanMag  = length(vd.mean);
        const float deltaMag = length(newSample - vd.mean);
        const float instInc  = saturate(deltaMag / max(meanMag, 0.01));
        vd.inconsistency = overwriteFrame ? instInc
                                          : lerp(vd.inconsistency, instInc, blendSpeed);

        // No mean-relative firefly clip here: it was suppressing legitimate
        // strong sources (e.g. a probe near an emissive lamp would have
        // tiny mean during burn-in, then a single ray hitting the lamp gave
        // ~2.0 emission, which the `min(sample, mean × 4)` clamp dropped to
        // near zero — emissive light never made it into the SH).
        // The trace shader's per-ray `min(radiance, 5.0)` cap is the only
        // firefly suppressor we need; it bounds energy without coupling to
        // the running mean. Asymmetric down-clipping (only suppress upward
        // spikes) was the right idea but the threshold model has to be
        // independent of mean magnitude — punted until that's worked out.

        vd.mean = overwriteFrame ? newSample
                                 : lerp(vd.mean, newSample, blendSpeed);

        g_Variance[vidx] = vd;
        texelMean = vd.mean;
        sharedTexels[inProbeTexel.y * kProbeSize + inProbeTexel.x] = texelMean;
#else
        if (sumWeight > 1e-4)
        {
            float2 newVal = float2(sumDist / sumWeight, sumDist2 / sumWeight);
            uint2 tile = DDGI_ProbeAtlasTile(DDGI_ProbeCoord(probeIdx, g_Vol), g_Vol, kProbeStride);
            uint2 px   = tile + uint2(1, 1) + inProbeTexel;
            float2 prev = g_Atlas[px];
            float2 outv = overwrite ? newVal : lerp(newVal, prev, g_Vol.hysteresis);
            g_Atlas[px] = outv;
        }
        else if (overwrite)
        {
            uint2 tile = DDGI_ProbeAtlasTile(DDGI_ProbeCoord(probeIdx, g_Vol), g_Vol, kProbeStride);
            uint2 px   = tile + uint2(1, 1) + inProbeTexel;
            g_Atlas[px] = float2(0, 0);
        }
#endif
    }

#if DDGI_RELIGHT_TARGET_IRRADIANCE
    GroupMemoryBarrierWithGroupSync();

    // Group-thread-0 collapses the 6×6 mean grid into one L1 SH probe and
    // EMA-blends with the previous frame's coefficients.
    if (lid.x == 0 && lid.y == 0)
    {
        DDGIProbeSH sh = DDGI_SH_Zero();
        for (uint y = 0; y < DDGI_IRRADIANCE_PROBE_SIZE; ++y)
        for (uint x = 0; x < DDGI_IRRADIANCE_PROBE_SIZE; ++x)
        {
            float3 dir = DDGI_DirectionFromTexel(uint2(x, y), DDGI_IRRADIANCE_PROBE_SIZE);
            float3 val = sharedTexels[y * DDGI_IRRADIANCE_PROBE_SIZE + x];
            sh = DDGI_SH_Add(sh, DDGI_SH_Project(dir, val));
        }
        // Normalise: SH integration over 36 uniformly-distributed texels with
        // total solid angle 4π.
        const float norm = (4.0 * DDGI_PI)
                         / float(DDGI_IRRADIANCE_PROBE_SIZE * DDGI_IRRADIANCE_PROBE_SIZE);
        sh = DDGI_SH_Multiply(sh, norm);

        // Write the SH directly — temporal stability comes from the per-texel
        // multi-scale mean estimator above. EMA-blending the SH on top would
        // double-smooth (estimator at ~3%/frame × hysteresis at ~3.5%/frame
        // ≈ 0.1% effective change per frame, making probes look frozen).
        // Match WickedEngine: trust the estimator, write SH straight.
        g_ProbeSH[probeIdx] = sh;
    }
#endif
}
