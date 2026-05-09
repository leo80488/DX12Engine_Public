// DecalApply.cs.hlsl — per-pixel clustered decal compositing, Unreal-style.
//
// Dispatch: (ceil(W/8), ceil(H/8), 1) with [numthreads(8,8,1)].
//
// Channel flow (per pixel, per decal):
//   1. Displacement  → parallax UV offset before all other samples
//   2. Opacity       → mask multiplier on the per-decal alpha
//   3. BaseColor     → GBuffer.Albedo  (with Cavity darkening + baseColorTint)
//   4. Normal + Bump → GBuffer.Normal  (RNM blend, normalStrength/bumpStrength scalars)
//   5. Roughness     → GBuffer.Surface.r   (tex.r × roughness scalar)
//   6. Specular      → GBuffer.Surface.a   (tex.r × specular  scalar)
//   7. AO            → GBuffer.Surface.b   (tex.r × ao        scalar)
//
// Each channel obeys a WRITE_* flag bit; slots without a texture use the
// paired scalar as a flat value.

#include "decal_common.hlsli"

cbuffer DecalApplyCB : register(b0, space2)
{
    float4x4 invViewProj;    // NDC → world (reconstruct world pos from depth)
    float4x4 invProj;        // NDC → view  (reconstruct view-space z for cluster lookup)
    float3   cameraPosWS;    // used for displacement parallax
    float    nearZ;
    float    farZ;
    uint     screenW;
    uint     screenH;
    uint     decalCount;     // unused here but kept to share CB with cull pass
    uint     debugMode;      // 0 = normal, 1 = cluster-count heatmap (albedo only)
    uint2    _pad1;
};

StructuredBuffer<GPUDecal>         g_Decals         : register(t1, space2);
Texture2D<float>                   g_SceneDepth     : register(t2, space2);
StructuredBuffer<DecalGridEntry>   g_DecalGrid      : register(t3, space2);
StructuredBuffer<uint>             g_DecalIndexList : register(t4, space2);

RWTexture2D<float4>                g_GBufferAlbedo  : register(u2, space2);
RWTexture2D<float4>                g_GBufferNormal  : register(u3, space2);
RWTexture2D<float4>                g_GBufferSurface : register(u4, space2);

Texture2D g_AllTextures[] : register(t0, space3);
SamplerState g_LinearSampler : register(s0, space2);

// ---------------------------------------------------------------------------
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndcXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 clip  = float4(ndcXY, depth, 1.0);
    float4 wh    = mul(clip, invViewProj);
    return wh.xyz / wh.w;
}
float ReconstructViewZ(float2 uv, float depth)
{
    float2 ndcXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 clip  = float4(ndcXY, depth, 1.0);
    float4 vh    = mul(clip, invProj);
    return vh.z / vh.w;
}

// Sample a grayscale channel (defaults to .r) from a bindless slot.
float SampleScalar(int idx, float2 uv)
{
    return g_AllTextures[NonUniformResourceIndex(idx)]
             .SampleLevel(g_LinearSampler, uv, 0).r;
}

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSApplyDecals(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pix = dtid.xy;
    if (pix.x >= screenW || pix.y >= screenH) return;

    float depth = g_SceneDepth.Load(int3(pix, 0));
    if (depth <= 0.0) return;   // reversed-Z: far plane = 0 (sky / cleared)

    float2 uv       = (float2(pix) + 0.5) / float2(screenW, screenH);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float  viewZ    = ReconstructViewZ(uv, depth);

    uint3 clusterCoord = GetClusterCoord(uv, viewZ, nearZ, farZ);
    uint  clusterIdx   = ClusterIndex(clusterCoord);
    DecalGridEntry entry = g_DecalGrid[clusterIdx];

    // ---- Debug heatmap path (skips actual decal work) ---------------------
    if (debugMode == 1)
    {
        const float t = saturate(float(entry.count) / float(MAX_DECALS_PER_CLUSTER));
        float3 heat;
        if (t < 0.5) heat = lerp(float3(0.0, 0.6, 0.0), float3(0.9, 0.9, 0.0), t * 2.0);
        else         heat = lerp(float3(0.9, 0.9, 0.0), float3(1.0, 0.0, 0.0), (t - 0.5) * 2.0);
        float4 cur = g_GBufferAlbedo[pix];
        g_GBufferAlbedo[pix] = float4(lerp(cur.rgb, heat, 0.5), cur.a);
        return;
    }

    if (entry.count == 0) return;

    // Load current GBuffer state.
    float4 albedoVal  = g_GBufferAlbedo [pix];
    float4 normalVal  = g_GBufferNormal [pix];
    float4 surfaceVal = g_GBufferSurface[pix];
    float3 normalWS   = normalize(normalVal.rgb * 2.0 - 1.0);

    // View direction in world space — used for displacement parallax.
    float3 viewDirWS = normalize(cameraPosWS - worldPos);

    [loop]
    for (uint i = 0; i < entry.count; ++i)
    {
        uint     decalIdx = g_DecalIndexList[entry.offset + i];
        GPUDecal d        = g_Decals[decalIdx];

        // World → decal-local [-0.5..0.5]^3.
        float3 localPos = mul(float4(worldPos, 1.0), d.worldToDecal).xyz;
        if (any(abs(localPos) > 0.5)) continue;

        // Angle fade — decal only affects surfaces facing "up into" the
        // projection axis (-decalForward).
        float ndotf     = dot(normalWS, -d.decalForwardWS);
        float angleFade = saturate((ndotf - d.angleFadeStart) /
                                    max(1.0 - d.angleFadeStart, 1e-4));
        if (angleFade <= 0.0) continue;

        float2 decalUV = localPos.xy + 0.5;

        // ---- (1) Displacement parallax --------------------------------------
        // Project view direction into decal-local space and shift the UV by
        // (height - 0.5) × scale along its XY projection. Offsets all
        // subsequent channel samples, so parallax is consistent across them.
        if (d.texDisplacement >= 0 && d.scalars1.w > 0.0)
        {
            float h = SampleScalar(d.texDisplacement, decalUV);
            float3 viewDirLocal = mul(float4(viewDirWS, 0.0), d.worldToDecal).xyz;
            decalUV += viewDirLocal.xy * (h - 0.5) * d.scalars1.w;
        }

        // ---- (2) Opacity mask -----------------------------------------------
        float opacityMask = 1.0;
        if (d.texOpacity >= 0)
            opacityMask = SampleScalar(d.texOpacity, decalUV);

        // Per-decal alpha = baseColorTint.a × opacity scalar × opacity tex
        //                  × angle fade.  Gate all subsequent blends.
        float alpha = d.baseColorTint.a * d.scalars0.x * opacityMask * angleFade;
        if (alpha <= 0.0) continue;

        uint flags = d.flags;

        // ---- (3) BaseColor (+ Cavity) ---------------------------------------
        if ((flags & DECAL_WRITE_BASECOLOR) && d.texBaseColor >= 0)
        {
            float4 bc = g_AllTextures[NonUniformResourceIndex(d.texBaseColor)]
                          .SampleLevel(g_LinearSampler, decalUV, 0);
            bc.rgb *= d.baseColorTint.rgb;

            // Cavity darkens BaseColor. cavitySample 1 → no change;
            // cavitySample 0 → bc.rgb * (1 - cavityStrength).
            if (d.texCavity >= 0 && d.scalars1.z > 0.0)
            {
                float cav = SampleScalar(d.texCavity, decalUV);
                bc.rgb *= lerp(1.0, cav, d.scalars1.z);
            }

            // When no dedicated Opacity texture, BaseColor's alpha acts as
            // the mask (common Unreal pattern). Matches the old behaviour.
            float effA = (d.texOpacity >= 0) ? alpha : alpha * bc.a;
            albedoVal.rgb = lerp(albedoVal.rgb, bc.rgb, saturate(effA));
        }

        // ---- (4) Normal + Bump ---------------------------------------------
        if (flags & DECAL_WRITE_NORMAL)
        {
            float3 nBlend = normalWS;
            bool   touched = false;

            if (d.texNormal >= 0)
            {
                float3 nTS = g_AllTextures[NonUniformResourceIndex(d.texNormal)]
                              .SampleLevel(g_LinearSampler, decalUV, 0).rgb * 2.0 - 1.0;
                nTS.xy *= d.scalars1.x;   // normalStrength
                nTS = normalize(nTS);
                nBlend = ReorientedNormalBlend(normalWS, nTS);
                touched = true;
            }

            if (d.texBump >= 0 && d.scalars1.y > 0.0)
            {
                // Normal-from-height via 3-tap forward-difference. Step size
                // is exactly one texel in the bump's own UV space — queried
                // from GetDimensions so the derivative is
                // resolution-independent. Derivation:
                //   T_u = (1, 0, (hx-h)/eps.x)
                //   T_v = (0, 1, (hy-h)/eps.y)
                //   N   = T_u × T_v = ((h-hx)/eps.x, (h-hy)/eps.y, 1)
                //
                // (h - hx)/eps.x is the slope dh/du in per-UV-unit terms,
                // so the resulting normal matches what a physically-based
                // height-to-normal conversion would produce. bumpStrength
                // then reads as "slope multiplier": 1.0 = 1:1 physical,
                // 0.1 = subtle, 2.0 = exaggerated.
                uint bw, bh;
                g_AllTextures[NonUniformResourceIndex(d.texBump)].GetDimensions(bw, bh);
                const float2 eps = 1.0 / float2(max(bw, 1u), max(bh, 1u));

                float h  = SampleScalar(d.texBump, decalUV);
                float hx = SampleScalar(d.texBump, decalUV + float2(eps.x, 0));
                float hy = SampleScalar(d.texBump, decalUV + float2(0, eps.y));

                float3 nB = normalize(float3(
                    (h - hx) / eps.x * d.scalars1.y,
                    (h - hy) / eps.y * d.scalars1.y,
                    1.0));

                if (touched) nBlend = ReorientedNormalBlend(nBlend, nB);
                else         nBlend = ReorientedNormalBlend(normalWS, nB);
                touched = true;
            }

            if (touched)
                normalWS = normalize(lerp(normalWS, nBlend, alpha));
        }

        // ---- (5) Roughness --------------------------------------------------
        if (flags & DECAL_WRITE_ROUGHNESS)
        {
            float r = d.scalars0.y;
            if (d.texRoughness >= 0)
                r *= SampleScalar(d.texRoughness, decalUV);
            surfaceVal.r = lerp(surfaceVal.r, saturate(r), alpha);
        }

        // ---- (6) Specular (dielectric reflectance → surface.a) --------------
        if (flags & DECAL_WRITE_SPECULAR)
        {
            float s = d.scalars0.z;
            if (d.texSpecular >= 0)
                s *= SampleScalar(d.texSpecular, decalUV);
            surfaceVal.a = lerp(surfaceVal.a, saturate(s), alpha);
        }

        // ---- (7) AO --------------------------------------------------------
        if (flags & DECAL_WRITE_AO)
        {
            float a_ = d.scalars0.w;
            if (d.texAO >= 0)
                a_ *= SampleScalar(d.texAO, decalUV);
            surfaceVal.b = lerp(surfaceVal.b, saturate(a_), alpha);
        }
    }

    // Write-back — preserve non-decal bytes (normal.a matIdx, surface.g metalness).
    g_GBufferAlbedo [pix] = albedoVal;
    g_GBufferNormal [pix] = float4(normalWS * 0.5 + 0.5, normalVal.a);
    g_GBufferSurface[pix] = surfaceVal;
}
