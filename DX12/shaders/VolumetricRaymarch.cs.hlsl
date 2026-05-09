// VolumetricRaymarch.cs.hlsl
// -----------------------------------------------------------------------------
// Per-pixel (half-resolution) raymarching for the high-quality volumetric
// lighting tier:
//   - Directional sun             (always, shadowed by CSM)
//   - Shadow-casting spot lights  (shadowed by SpotShadowPass atlas)
//
// Complements FroxelLightInject.cs.hlsl, which handles "many small lights"
// cheaply at 3D-grid resolution. The grid can't resolve sharp CSM cascade
// or spot-atlas shadow boundaries; this pass reads them at per-pixel
// precision and delivers crisp light shafts + occluder silhouettes.
//
// Output: half-res R11G11B10/RGBA16F texture.
//   .rgb = accumulated in-scatter radiance along view ray
//   .a   = transmittance at scene depth (used by Apply PS for fog extinction)
// -----------------------------------------------------------------------------

#include "FroxelCommon.hlsli"
#include "cluster_common.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture3D<float4>            FroxelDensityTex : register(t0, space2);
Texture2DArray<float>        ShadowCascades   : register(t1, space2);
StructuredBuffer<GPULight>   Lights           : register(t2, space2);
Texture2D<float>             SceneDepth       : register(t3, space2);
Texture2DArray<float>        SpotShadowAtlas  : register(t6, space2);
StructuredBuffer<float4x4>   SpotShadowVPs    : register(t7, space2);

SamplerState                 LinearSampler    : register(s0, space2);
SamplerComparisonState       ShadowSampler    : register(s1, space2);

RWTexture2D<float4>          InScatterOut     : register(u0, space2);

// -- Tunable budget ----------------------------------------------------------
// 32 steps along the view ray is the sweet spot for ~1 ms at 960×540 on a
// mid-range GPU. Each step samples the froxel density once, evaluates the
// sun once (CSM tap), and loops over shadow-casting spot lights. Temporal
// reprojection smooths residual noise; blue-noise jitter turns banding into
// high-freq noise that the temporal pass averages out.
#define RM_STEPS          32
// Limit spot-light taps per ray step to avoid worst-case blowup when many
// spot lights overlap. In practice kMaxCasters = 8 (see SpotShadowPass), so
// this is just defence-in-depth.
#define RM_MAX_SPOTS      8

// ---------------------------------------------------------------------------
// CSM — same "first in-bounds cascade wins" strategy used by FroxelLightInject
// pre-change. Returns visibility in [0, 1].
// ---------------------------------------------------------------------------
float SampleCascade(int cascade, float4x4 m, float3 worldPos, float texelSize, float bias)
{
    float4 sp = mul(float4(worldPos, 1.0), m);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        sp.z < 0.0 || sp.z > 1.0) return -1.0;

    float depth = sp.z + bias;
    float shadow = 0.0;
    [unroll] for (int dy = 0; dy <= 1; ++dy)
    [unroll] for (int dx = 0; dx <= 1; ++dx)
    {
        float2 o = (float2(dx, dy) - 0.5) * texelSize;
        shadow += ShadowCascades.SampleCmpLevelZero(
            ShadowSampler, float3(uv + o, float(cascade)), depth);
    }
    return shadow * 0.25;
}

float ComputeSunShadow(float3 worldPos)
{
    float4x4 mats[3] = { P.shadowMatrix0, P.shadowMatrix1, P.shadowMatrix2 };
    [unroll] for (int c = 0; c < 3; ++c)
    {
        float s = SampleCascade(c, mats[c], worldPos,
                                P.shadowParams.x, P.shadowParams.z);
        if (s >= 0.0) return s;
    }
    return 1.0;
}

// ---------------------------------------------------------------------------
// Spot-shadow atlas sample for raymarched in-scatter.
//
// Sampler state (set in CreateComputeRootSignature, s1 space2):
//   ADDRESS_MODE_BORDER + OPAQUE_WHITE + GREATER_EQUAL compare. Under
//   reversed-Z, border → SM.z = 1.0, compare against any receiver.z ≤ 1.0
//   returns 0 (SHADOWED). So out-of-UV-bounds taps are automatically
//   rejected by the sampler; we do NOT need a manual UV clamp that
//   would otherwise bleed past the cone edge.
//
// Key fixes vs the earlier version (all caused visible leaks):
//   1. Removed the `sc.z > 1.0` → shadowed rejection. In reversed-Z that
//      branch marked voxels *closer to the light than the SM near plane*
//      as occluded — i.e. killed light near the spot's apex entirely.
//      Points past the near plane can never have an occluder ahead of
//      them in the shadow map, so returning 1.0 (fully lit) is correct.
//   2. Removed the +0.00025 receiver bias. It was copied from the
//      surface-shadow path (where it cancels self-shadowing acne). For
//      volumetric samples suspended in the medium the same bias just
//      pushes voxels one step closer to the light than they really are,
//      and voxels sitting just *behind* a thin wall leak through.
//   3. Dropped the manual 2×2 tap loop. The sampler's LINEAR comparison
//      filter already does a 4-texel bilinear PCF per SampleCmp — the
//      extra ±0.5 texel offsets I was adding produced a 3×3 footprint
//      of variable weights that amplified edge noise near silhouettes.
// ---------------------------------------------------------------------------
float SpotShadowVisibility(float3 worldPos, uint sliceIdx)
{
    float4 sc = mul(float4(worldPos, 1.0), SpotShadowVPs[sliceIdx]);
    // Behind the light's apex plane — no path from the light.
    if (sc.w <= 0.0) return 0.0;
    sc.xyz /= sc.w;

    // Closer than the shadow frustum's near plane — can't be occluded by
    // anything the SM could possibly have recorded. Treat as fully lit.
    if (sc.z > 1.0) return 1.0;

    float2 uv = sc.xy * float2(0.5, -0.5) + 0.5;
    // One SampleCmp with the LINEAR comparison sampler. BORDER+WHITE handles
    // UV outside [0,1] (returns 0 shadow) and sc.z outside [0,1] on the far
    // side (sc.z < 0 → receiver-less-than-any-stored-depth → 0 shadow).
    return SpotShadowAtlas.SampleCmpLevelZero(
        ShadowSampler, float3(uv, (float)sliceIdx), sc.z);
}

// ---------------------------------------------------------------------------
// Interleaved-gradient noise — cheap hash with a good low-discrepancy pattern
// across a screen tile. Used to jitter the first ray step so the temporal
// pass can converge on a smooth integral instead of quantised plateaus.
// Incorporates frameIndex so successive frames supply fresh samples.
// ---------------------------------------------------------------------------
float IGN(float2 pixel, uint frameIdx)
{
    pixel += float(frameIdx & 63u) * 5.588238;  // decorrelate across frames
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// ---------------------------------------------------------------------------
// Reconstruct the world-space ray for a full-res UV + the world distance to
// the scene surface at that pixel. Uses invViewProj end-to-end so the result
// is independent of the depth convention (standard / reversed-Z / inf-far) —
// whatever viewProj was on the C++ side, invViewProj inverts it.
// ---------------------------------------------------------------------------
void BuildViewRay(float2 uv, out float3 rayOrigin, out float3 rayDir,
                  out float maxT)
{
    rayOrigin = P.cameraPos;

    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    // Ray direction: from camera through any point along this pixel's column
    // in NDC. We pick ndc.z = 0.5 (arbitrary non-degenerate) and normalize.
    float4 pH = mul(float4(ndc, 0.5, 1.0), P.invViewProj);
    float3 pW = pH.xyz / pH.w;
    rayDir = normalize(pW - rayOrigin);

    float ndcZ = SceneDepth.SampleLevel(LinearSampler, uv, 0).r;
    // Reversed-Z: sky writes ndc.z = 0. (Standard-Z would write 1.) We assume
    // reversed-Z to match the engine, but the surface path below uses only
    // invViewProj — it works either way. Only the sky early-out is
    // convention-specific.
    if (ndcZ <= 1e-6)
    {
        maxT = P.froxelFar;
        return;
    }

    float4 surfH = mul(float4(ndc, ndcZ, 1.0), P.invViewProj);
    float3 surfW = surfH.xyz / surfH.w;
    maxT = min(length(surfW - rayOrigin), P.froxelFar);
}

// ---------------------------------------------------------------------------
// Sample the froxel density 3D texture at a given world position. Returns
// (scatteringRGB, extinction). When the sample falls outside the froxel
// grid (distance > froxelFar, etc.) we return a zero-density/zero-scatter
// value so the caller contributes nothing at that step.
// ---------------------------------------------------------------------------
float4 SampleFroxelDensity(float3 worldPos)
{
    float3 uvw = WorldToFroxelUVW(worldPos, P);
    if (any(uvw < 0.0) || any(uvw > 1.0))
        return float4(0, 0, 0, 0);
    return FroxelDensityTex.SampleLevel(LinearSampler, uvw, 0);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint halfW, halfH;
    InScatterOut.GetDimensions(halfW, halfH);
    if (DTid.x >= halfW || DTid.y >= halfH) return;

    // UV at centre of the 2×2 full-res block that this half-res pixel maps to.
    float2 uv = (float2(DTid.xy) + 0.5) / float2(halfW, halfH);

    float3 rayOrigin, rayDir;
    float  maxT;
    BuildViewRay(uv, rayOrigin, rayDir, maxT);

    // Start / end of the march, both in world-space distance along rayDir.
    // (Matching units — the previous revision mixed world distance with
    //  view-space Z, which put samples at wrong world positions for any
    //  off-axis pixel and caused shadow-casting spot lights to appear in
    //  the wrong place.)
    float tMin = P.froxelNear;
    float tMax = maxT;
    if (tMax <= tMin) { InScatterOut[DTid.xy] = float4(0, 0, 0, 1); return; }

    float stepLen  = (tMax - tMin) / float(RM_STEPS);
    float jitter   = IGN(float2(DTid.xy), P.frameIndex);  // [0, 1)
    float t        = tMin + jitter * stepLen;

    // View direction at the camera (for HG phase). Same direction for every
    // step along the ray, so hoisted out of the loop.
    float3 V        = rayDir;
    float3 toSun    = -normalize(P.sunDir);
    float  sunCos   = dot(V, toSun);
    float  sunPhase = PhaseHG(sunCos, P.anisotropy);

    uint   spotCount = (uint)(P.shadowParams.y);
    float3 inScatter = float3(0, 0, 0);
    float  trans     = 1.0;

    [loop] for (int i = 0; i < RM_STEPS; ++i, t += stepLen)
    {
        float3 p = rayOrigin + rayDir * t;

        float4 dens = SampleFroxelDensity(p);
        float  sigmaT = dens.a;
        if (sigmaT <= 0.0) continue;

        // ---- Sun contribution ------------------------------------------------
        float3 stepL = float3(0, 0, 0);
        if (P.sunStrength > 0.0)
        {
            float sunVis = ComputeSunShadow(p);
            stepL += P.sunColor * P.sunStrength * sunVis * sunPhase;
        }

        // ---- Shadow-casting spot lights -------------------------------------
        // Iterate the opted-in volumetric light list. Skip lights without a
        // shadow atlas slice — those are FroxelLightInject's domain.
        uint processed = 0;
        [loop] for (uint li = 0; li < spotCount && processed < RM_MAX_SPOTS; ++li)
        {
            GPULight L = Lights[li];
            if (L.type != 2u)                       continue;  // spots only (no point atlas yet)
            if (L.shadowSliceIdx == 0xFFFFFFFFu)    continue;  // no shadow → froxel tier

            float3 toL   = L.position - p;
            float  dist  = length(toL);
            if (dist > L.radius) continue;
            float3 Ldir  = toL / max(dist, 1e-4);

            // Same windowed distance falloff as froxel path (avoid 1/d² blow-up).
            float distNorm  = dist / max(L.radius, 1e-4);
            float distNorm2 = distNorm * distNorm;
            float atten     = saturate(1.0 - distNorm2 * distNorm2);
            atten *= atten;

            // Spot cone: soft smoothstep from inner 0.7× angle to outer edge.
            float cosA     = dot(-Ldir, normalize(L.direction));
            float cosOuter = cos(L.spotAngle);
            float cosInner = cos(L.spotAngle * 0.70);
            float coneFall = smoothstep(cosOuter, cosInner, cosA);
            if (coneFall <= 0.0) continue;

            float  spotCos   = dot(V, Ldir);
            float  spotPhase = PhaseHG(spotCos, P.anisotropy);
            float  shadowVis = SpotShadowVisibility(p, L.shadowSliceIdx);

            stepL += L.color * L.intensity *
                     atten * coneFall * spotPhase * shadowVis;
            ++processed;
        }

        // Beer's law transmittance over this step + integrate in-scatter using
        // Frostbite-style analytical integration for constant σ over the step
        // (stable under varying step lengths, no double-counting).
        float stepTrans = exp(-sigmaT * stepLen);
        float3 sInt     = (stepL - stepL * stepTrans) / max(sigmaT, 1e-4);
        inScatter      += trans * dens.rgb * sInt;
        trans          *= stepTrans;

        // Early-out when the view ray has become fully opaque — further steps
        // can't contribute meaningful in-scatter.
        if (trans < 0.01) break;
    }

    InScatterOut[DTid.xy] = float4(inScatter, trans);
}
