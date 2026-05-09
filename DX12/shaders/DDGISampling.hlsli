// DDGISampling.hlsli — included by Lighting.ps to sample DDGI at shading time.
//
// Probe storage: L1 SH (StructuredBuffer<DDGIProbeSH>, one entry per probe).
// Depth storage: octahedral atlas (Texture2D<float2>) for chebyshev visibility.
//
// Trilinear-with-chebyshev visibility per the Majercik 2019 paper, but the
// per-probe irradiance lookup is now `DDGI_SH_Irradiance(N)` rather than a
// bilinear octahedral atlas sample — same shading semantics, much cheaper
// per-probe + no octahedral seam.

#ifndef DDGI_SAMPLING_HLSLI
#define DDGI_SAMPLING_HLSLI

#include "DDGICommon.hlsli"

// Convert a direction → UV inside the probe depth tile (with the +1 texel
// border accounted for).
float2 DDGI_TileUV(float3 dir, uint probeSize, uint probeStride)
{
    float2 oct = DDGI_OctEncode(dir) * 0.5 + 0.5; // [0,1]
    float2 tex = oct * float(probeSize) + 1.0;
    return tex / float(probeStride);
}

// Sample one probe's depth atlas tile via bilinear interpolation along the
// octahedral mapping of `dir`.
float2 DDGI_SampleProbeDepthUV(Texture2D<float2> atlas,
                               SamplerState sampLinear,
                               int3 probeCoord,
                               DDGIVolumeGPU vol,
                               float3 dir)
{
    const uint probeSize   = DDGI_DEPTH_PROBE_SIZE;
    const uint probeStride = DDGI_DEPTH_PROBE_STRIDE;
    uint2  tile = DDGI_ProbeAtlasTile(probeCoord, vol, probeStride);
    float2 tileUV = DDGI_TileUV(dir, probeSize, probeStride);

    uint w, h;
    atlas.GetDimensions(w, h);
    float2 uv = (float2(tile) + tileUV * float(probeStride)) / float2(w, h);
    return atlas.SampleLevel(sampLinear, uv, 0).rg;
}

// Main entry — sample DDGI for a shading point.
//
// Returns a float4: rgb = irradiance, a = total volume coverage weight in
// [0..1]. Caller falls back to Sky IBL diffuse for the (1-w) remainder.
float4 DDGI_SampleVolume(float3 worldPos,
                         float3 normal,
                         float3 viewDir,
                         DDGIVolumeGPU vol,
                         StructuredBuffer<DDGIProbeSH> probeSH,
                         Texture2D<float2> depthAtlas,
                         StructuredBuffer<DDGIProbeData> probeData,
                         SamplerState sampLinear)
{
    float volumeWeight = DDGI_VolumeFadeWeight(worldPos, vol);
    if (volumeWeight <= 1e-4) return float4(0, 0, 0, 0);

    float3 biasedPos = worldPos + normal * vol.normalBias + viewDir * vol.viewBias;

    float3 g     = DDGI_GridSpace(biasedPos, vol);
    int3   base  = int3(floor(g));
    float3 alpha = saturate(frac(g));

    base.x = clamp(base.x, 0, (int)vol.probeCountsX - 2);
    base.y = clamp(base.y, 0, (int)vol.probeCountsY - 2);
    base.z = clamp(base.z, 0, (int)vol.probeCountsZ - 2);

    float3 sumIrr = 0;
    float  sumW   = 0;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        int3 off = int3((i >> 0) & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 c   = base + off;

        if (any(c < 0) ||
            c.x >= (int)vol.probeCountsX ||
            c.y >= (int)vol.probeCountsY ||
            c.z >= (int)vol.probeCountsZ) continue;

        uint probeIdx = DDGI_ProbeIndex(c, vol);
        DDGIProbeData pd = probeData[probeIdx];
        if (pd.state == DDGI_PROBE_STATE_INACTIVE) continue;

        float3 probePos = DDGI_ProbeWorldPos(c, vol) + pd.offset;
        float3 dirToProbe = probePos - biasedPos;
        float  distToProbe = length(dirToProbe);
        if (distToProbe < 1e-4) { dirToProbe = normal; distToProbe = 1.0; }
        else                    { dirToProbe /= distToProbe; }

        // Trilinear weight.
        float3 trilinear = lerp(1.0 - alpha, alpha, float3(off));
        float  w = trilinear.x * trilinear.y * trilinear.z;

        // Backface weight — soft, avoids "behind-the-wall" probes.
        float bf = (dot(dirToProbe, normal) + 1.0) * 0.5;
        w *= max(0.05, bf * bf);

        // Chebyshev visibility via depth atlas. Skip the test entirely when
        // the depth atlas hasn't been written yet (mean ≈ 0): with mean = 0
        // and any real probe distance, the formula evaluates to occlusion = 0
        // and the gate below would zero every probe, making every freshly-
        // allocated volume "fully occluded" until depth converges (~3
        // overwrite frames). Better to trust the trilinear+backface weights
        // alone for that brief warm-up window.
        float2 d = DDGI_SampleProbeDepthUV(depthAtlas, sampLinear, c, vol, -dirToProbe);
        float mean = d.x;
        if (mean > 1e-3)
        {
            // Variance = E[d²] - E[d]² (textbook orientation; abs() handles
            // tiny float drift only, not a sign flip).
            float variance = abs(d.y - mean * mean);
            float chebDist = max(distToProbe - mean, 0.0);
            float occlusion = variance / (variance + chebDist * chebDist);
            occlusion = max(occlusion * occlusion * occlusion, 0.0);
            if (distToProbe > mean) w *= occlusion;
        }

        // Irradiance from L1 SH at the surface normal.
        float3 irr = DDGI_SH_Irradiance(probeSH[probeIdx], normal);
        sumIrr += irr * w;
        sumW   += w;
    }

    if (sumW < 1e-4) return float4(0, 0, 0, 0);

    float3 irradiance = sumIrr / sumW;
    irradiance *= vol.diffuseTint * vol.diffuseScale;

    return float4(irradiance, volumeWeight);
}

#endif // DDGI_SAMPLING_HLSLI
