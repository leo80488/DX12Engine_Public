// XeGTAODepthPrefilter.cs.hlsl — VERBATIM port of
// XeGTAO_PrefilterDepths16x16 from Intel XeGTAO.hlsli (SPDX MIT).
//
// Writes all 5 mips of the linear depth pyramid in ONE dispatch using
// groupshared communication between threads. Thread group is 8×8 (matches
// reference); each thread loads a 2×2 quad from hardware depth → mip 0, then
// tiered reductions via `g_scratchDepths` produce mips 1..4.
//
// Engine-specific bindings:
//   b0 space2 — PrefilterCB (viewport size, DepthUnpackConsts, falloff knobs)
//   t0 space2 — hardware depth (source)
//   u0..u4    — mip 0..4 of the linear depth pyramid (R32F)

cbuffer PrefilterCB : register(b0, space2)
{
    uint2  SrcSize;              // unused at shader side (hardware depth dims)
    uint2  DstSize;              // unused
    float2 DepthUnpackConsts;    // XeGTAO DepthUnpackConsts (mul, add)
    float  EffectRadius;
    float  EffectFalloffRange;
    float  RadiusMultiplier;
    uint   SrcMip;               // unused in this variant
};

Texture2D<float>    gSrcDepth : register(t0, space2);

RWTexture2D<float>  gOutMip0 : register(u0, space2);
RWTexture2D<float>  gOutMip1 : register(u1, space2);
RWTexture2D<float>  gOutMip2 : register(u2, space2);
RWTexture2D<float>  gOutMip3 : register(u3, space2);
RWTexture2D<float>  gOutMip4 : register(u4, space2);

SamplerState        gPointClamp : register(s0, space2);  // NB: sampler is LINEAR,
                                                         // used with GatherRed
                                                         // (gather is format-
                                                         // invariant on sampler
                                                         // filtering).

// ---------------------------------------------------------------------------
// Helpers (verbatim from XeGTAO.hlsli).
// ---------------------------------------------------------------------------
float XeGTAO_ScreenSpaceToViewSpaceDepth(float screenDepth)
{
    return DepthUnpackConsts.x / (DepthUnpackConsts.y - screenDepth);
}

float XeGTAO_ClampDepth(float d)
{
    // FP32 path (reference supports FP16 via min16float; we use full float).
    return clamp(d, 0.0, 3.402823466e+38);
}

// Weighted 4-sample average that favours the near-surface sample — preserves
// thin occluders so GTAO can't tunnel past them at coarse mips.
float XeGTAO_DepthMIPFilter(float depth0, float depth1, float depth2, float depth3)
{
    float maxDepth = max(max(depth0, depth1), max(depth2, depth3));

    const float depthRangeScaleFactor = 0.75;
    const float effectRadius = depthRangeScaleFactor * EffectRadius * RadiusMultiplier;
    const float falloffRange = EffectFalloffRange * effectRadius;
    const float falloffFrom  = effectRadius * (1.0 - EffectFalloffRange);
    const float falloffMul   = -1.0 / falloffRange;
    const float falloffAdd   = falloffFrom / falloffRange + 1.0;

    float w0 = saturate((maxDepth - depth0) * falloffMul + falloffAdd);
    float w1 = saturate((maxDepth - depth1) * falloffMul + falloffAdd);
    float w2 = saturate((maxDepth - depth2) * falloffMul + falloffAdd);
    float w3 = saturate((maxDepth - depth3) * falloffMul + falloffAdd);

    float wSum = w0 + w1 + w2 + w3;
    return (wSum > 1e-6)
        ? (w0*depth0 + w1*depth1 + w2*depth2 + w3*depth3) / wSum
        : maxDepth;
}

// ---------------------------------------------------------------------------
// Main entry — 8×8 threads, groupshared depth cache reused for mip 1..4.
// VERBATIM port of XeGTAO_PrefilterDepths16x16 (Intel XeGTAO.hlsli).
// ---------------------------------------------------------------------------
groupshared float g_scratchDepths[8][8];

[numthreads(8, 8, 1)]
void CSPrefilter(uint3 dispatchThreadID : SV_DispatchThreadID,
                 uint3 groupThreadID    : SV_GroupThreadID)
{
    // Each thread produces 4 texels in mip 0 — a 2×2 quad at (baseCoord*2).
    const uint2 baseCoord = dispatchThreadID.xy;
    const uint2 pixCoord  = baseCoord * 2;

    // GatherRed reads a 2×2 footprint. The reference offsets the gather UV so
    // the returned quad maps to (x+0,y+0), (x+1,y+0), (x+0,y+1), (x+1,y+1).
    // GatherRed order: .w = (0,0), .z = (1,0), .x = (0,1), .y = (1,1)  — hence
    // the remapping below matches the reference verbatim.
    const float2 pixelSize = 1.0 / float2(SrcSize);
    float2 uv = (float2(pixCoord) + 1.0) * pixelSize;
    float4 depths4 = gSrcDepth.GatherRed(gPointClamp, uv);

    float depth0 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.w));
    float depth1 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.z));
    float depth2 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.x));
    float depth3 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.y));

    gOutMip0[pixCoord + uint2(0, 0)] = depth0;
    gOutMip0[pixCoord + uint2(1, 0)] = depth1;
    gOutMip0[pixCoord + uint2(0, 1)] = depth2;
    gOutMip0[pixCoord + uint2(1, 1)] = depth3;

    // Mip 1 — one sample per thread.
    float dm1 = XeGTAO_DepthMIPFilter(depth0, depth1, depth2, depth3);
    gOutMip1[baseCoord] = dm1;
    g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm1;

    GroupMemoryBarrierWithGroupSync();

    // Mip 2 — one thread in every 2×2 block.
    [branch]
    if (all((groupThreadID.xy % 2) == 0))
    {
        float inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_scratchDepths[groupThreadID.x + 1][groupThreadID.y + 0];
        float inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 1];
        float inBR = g_scratchDepths[groupThreadID.x + 1][groupThreadID.y + 1];

        float dm2 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR);
        gOutMip2[baseCoord / 2] = dm2;
        g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm2;
    }

    GroupMemoryBarrierWithGroupSync();

    // Mip 3 — one thread in every 4×4 block.
    [branch]
    if (all((groupThreadID.xy % 4) == 0))
    {
        float inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_scratchDepths[groupThreadID.x + 2][groupThreadID.y + 0];
        float inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 2];
        float inBR = g_scratchDepths[groupThreadID.x + 2][groupThreadID.y + 2];

        float dm3 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR);
        gOutMip3[baseCoord / 4] = dm3;
        g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm3;
    }

    GroupMemoryBarrierWithGroupSync();

    // Mip 4 — one thread in every 8×8 block (one thread per group).
    [branch]
    if (all((groupThreadID.xy % 8) == 0))
    {
        float inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_scratchDepths[groupThreadID.x + 4][groupThreadID.y + 0];
        float inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 4];
        float inBR = g_scratchDepths[groupThreadID.x + 4][groupThreadID.y + 4];

        float dm4 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR);
        gOutMip4[baseCoord / 8] = dm4;
    }
}
