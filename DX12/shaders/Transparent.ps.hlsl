// Transparent.ps.hlsl — Forward PBR pixel shader for alpha-blended geometry.
//
// Called by TransparentPass after GBuffer + Lighting + Skybox.
// Vertices come from GBuffer.vs.hlsl (same PVF layout, provides worldPos interpolant).
//
// Root bindings (must match TransparentPass.cpp):
//   b0 space0  — PushConstants (meshDescIdx, instanceOffset, materialIndex)
//   b2 space0  — LightCB (lighting, IBL params)        ← slot 1, NOT b1 (VS owns b1=PerViewCB)
//   t2 space0  — StructuredBuffer<MaterialGPUData>
//   t3 space0  — g_BaseColor  (Texture2D)
//   t4 space0  — g_SurfaceMap (Texture2D)
//   t5 space0  — g_NormalMap  (Texture2D, BC5 RG — B reconstructed)
//   t6 space0  — gIrradiance  (TextureCube)
//   t7 space0  — gRadiance    (TextureCube)
//   t8 space0  — gBRDFLUT     (Texture2D<float2>)
//   s0 space0  — linear-wrap sampler (material textures)
//   s1         — trilinear-wrap sampler (IBL)
//
// Output:
//   SV_TARGET0 = float4(rgb=lit HDR radiance, a=surface alpha)
//   Alpha is used by hardware blend state. Blend mode is encoded in the PSO,
//   not in the shader — the same PS handles Alpha, Additive, Premultiplied,
//   and Multiply. Premultiplied is the one case that needs shader cooperation:
//   its blend state (ONE / INV_SRC_ALPHA) expects rgb to already be multiplied
//   by alpha, so the return path pre-multiplies under #if PREMULTIPLIED_BLEND.

#include "material.hlsli"
#include "sky_sh.hlsli"            // gSkySH SRV + EvalSH2
#include "reflection_probe.hlsli"  // ReflectionProbe struct,
                                   // gReflectionProbeArray / gReflectionProbes SRVs,
                                   // ParallaxCorrectAABB / ComputeProbeWeight helpers

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

// LightCB at b2 — the GBuffer VS owns b1 for PerViewCB; both share the same
// root signature. Forward transparent reads only a subset of the LightCB
// fields (lightDir, lightColor, cameraPos, ambient, iblRadianceMips,
// iblStrength, iblUseSH, reflectionProbeCount), but it sees the full layout
// so that any future C++ addition stays automatically in sync — the shader's
// dead reads are optimised away by DXC.
#define LIGHT_CB_REGISTER b2
#include "light_cb.hlsli"

StructuredBuffer<MaterialGPUData> g_Materials  : register(t2, space0);
Texture2D<float4>                 g_BaseColor  : register(t3, space0);
Texture2D<float4>                 g_SurfaceMap : register(t4, space0);
Texture2D<float4>                 g_NormalMap  : register(t5, space0);
TextureCube                       gIrradiance  : register(t6, space0);
TextureCube                       gRadiance    : register(t7, space0);
Texture2D<float2>                 gBRDFLUT     : register(t8, space0);

// (gSkySH at t19, gReflectionProbeArray at t23, gReflectionProbes at t24
// come from the included headers.)

// Bindless texture array — indexed by MaterialGPUData.textureHandleIds[].
Texture2D g_AllTextures[] : register(t0, space2);

SamplerState g_LinearWrap : register(s0, space0);
SamplerState gIBLSampler  : register(s1);

// ---- Vertex interpolants (match GBuffer.vs.hlsl output) --------------------
struct PSIn
{
    float4 sv       : SV_POSITION;
    float3 worldPos : POSITIONWS;
    float2 uv       : TEXCOORD0;
    float3 wn       : NORMAL;
    float3 wt       : TANGENT;
    float3 wbt      : BINORMAL;
    float3 col      : COLOR;
};

#include "brdf.hlsli"

// ---------------------------------------------------------------------------
float4 main(PSIn i) : SV_TARGET
{
#if UNLIT
    // Unlit billboard: sample base color texture and output directly (no PBR).
    float4 texColor = g_BaseColor.Sample(g_LinearWrap, i.uv);
    clip(texColor.a - 0.01);
    return texColor;
#endif

    MaterialGPUData mat = g_Materials[materialIndex];

    float4 baseColor      = mat.baseColor;
    float  roughnessMin   = mat.roughnessMin;
    float  roughnessMax   = mat.roughnessMax;
    float  metalnessMin   = mat.metalnessMin;
    float  metalnessMax   = mat.metalnessMax;
    float  reflectance    = mat.reflectance;
    float  normalStrength = mat.normalStrength;

    bool useFallback = (mat.paramCount == 0);
    if (useFallback)
    {
        baseColor      = float4(i.col, 1.0);
        roughnessMin   = 0.0; roughnessMax  = 0.5;
        metalnessMin   = 0.0; metalnessMax  = 0.0;
        reflectance    = 0.04;
        normalStrength = 1.0;
    }

    // Base color + alpha (alpha drives hardware blending)
    baseColor *= g_BaseColor.Sample(g_LinearWrap, i.uv);
    float surfaceAlpha = baseColor.a;

    // Surface map
    float4 surface  = g_SurfaceMap.Sample(g_LinearWrap, i.uv);
    float  ao        = surface.r;
    float  roughness = max(lerp(roughnessMin, roughnessMax, surface.g), 0.045);
    float  metalness = lerp(metalnessMin, metalnessMax, surface.b);

    // Normal map — BC5: RG stored, reconstruct B.
    float2 rg       = g_NormalMap.Sample(g_LinearWrap, i.uv).rg * 2.0 - 1.0;
    float  nz       = sqrt(saturate(1.0 - dot(rg, rg)));
    float3 tsNormal = float3(rg, nz);
    tsNormal.xy    *= normalStrength;
    tsNormal        = normalize(tsNormal);

    float3 N  = normalize(i.wn);
    float3 T  = normalize(i.wt);
    float3 BT = normalize(i.wbt);
    float3x3 TBN = float3x3(T, BT, N);
    N = normalize(mul(tsNormal, TBN));

    // Geometric specular AA (Kaplanyan & Hable 2016) — see GBuffer.ps.hlsl
    // for full rationale. Widens roughness by the sub-pixel normal variance
    // so rough-ish surfaces don't produce single-pixel sparkle under jitter.
    {
        const float kSigma2 = 0.25;
        const float kKappa  = 0.18;
        float3 dndu     = ddx(N);
        float3 dndv     = ddy(N);
        float  variance = kSigma2 * (dot(dndu, dndu) + dot(dndv, dndv));
        float  kernelR2 = min(2.0 * variance, kKappa);
        float  alpha    = roughness * roughness;
        float  alpha2   = saturate(alpha + kernelR2);
        roughness       = sqrt(alpha2);
    }

    // Lighting vectors
    float3 V     = normalize(cameraPos - i.worldPos);
    float3 L     = normalize(-lightDir);
    float3 H     = normalize(V + L);
    float  NdotL = saturate(dot(N, L));
    float  NdotV = saturate(dot(N, V));

    float  f0Scalar = 0.16 * reflectance * reflectance;
    float3 F0       = lerp(float3(f0Scalar, f0Scalar, f0Scalar), baseColor.rgb, metalness);

    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
    float  D = DistributionGGX(N, H, roughness);
    float  G = GeometrySmith(NdotV, NdotL, roughness);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kD       = (1.0 - F) * (1.0 - metalness);
    float3 diffuse  = kD * baseColor.rgb / PI;
    // Split direct lighting into body (absorbed by glass) vs surface (Fresnel).
    float3 directDiffuse  = diffuse  * lightColor * NdotL;
    float3 directSpecular = specular * lightColor * NdotL;

    // IBL — mirrors the deferred LightingPass diffuse + reflection-probe path
    // so transparent surfaces pick up the same procedural sky / probes the
    // opaque pixels do (otherwise glass under TOD/atmosphere shows the loaded
    // .itex skybox while the rest of the scene shows the procedural sky —
    // user-visible mismatch).
    float3 iblDiffuseTerm  = float3(0, 0, 0);
    float3 iblSpecularTerm = float3(0, 0, 0);
    // Post-DDGI semantic: iblStrength scales SPECULAR only; diffuse uses
    // skyIBLDiffuseScale. Diffuse runs whenever the radiance cube is present
    // so SH/cube-irradiance still lights transparent surfaces even when
    // iblStrength = 0. Transparent surfaces don't sample DDGI volumes
    // (sparse pixel coverage isn't worth the multi-volume binding churn).
    [branch]
    if (iblRadianceMips > 0)
    {
        // Diffuse — SH evaluation (procedural sky) when SkyIBLPass has projected
        // the cube; otherwise sample the static irradiance cubemap (preset .itex).
        float3 iblDiffuse  = (iblUseSH != 0) ? EvalSH2(N)
                                             : gIrradiance.Sample(gIBLSampler, N).rgb;

        float3 R   = reflect(-V, N);
        float  mip = roughness * float(iblRadianceMips - 1);

        // Specular — weighted reflection-probe accumulation, sky cube fallback
        // for the remaining (1 - probeWeight). Iterates ALL active probes
        // (no cluster lookup): forward transparent pixels are typically a small
        // count and would need viewMatrix+clusterNearZ/FarZ in this CB to do
        // the screen-space cluster index — not worth the binding churn.
        float3 probeAccum  = float3(0, 0, 0);
        float  probeWeight = 0.0;
        for (uint pi = 0; pi < reflectionProbeCount && probeWeight < 0.999; ++pi)
        {
            ReflectionProbe probe = gReflectionProbes[pi];
            float w = ComputeProbeWeight(i.worldPos, probe) * (1.0 - probeWeight);
            if (w <= 1e-3) continue;
            float3 Rcorr   = ParallaxCorrectAABB(R, i.worldPos, probe.position,
                                                 probe.boxMin, probe.boxMax);
            float4 sample4 = gReflectionProbeArray.SampleLevel(gIBLSampler,
                                 float4(Rcorr, (float)probe.cubemapSlice), mip);
            probeAccum  += sample4.rgb * w;
            probeWeight += w;
        }
        float3 skySpecular = gRadiance.SampleLevel(gIBLSampler, R, mip).rgb;
        float3 iblSpecular = probeAccum + skySpecular * (1.0 - probeWeight);

        float3 Fibl    = FresnelSchlickRoughness(NdotV, F0, roughness);
        float2 envBRDF = gBRDFLUT.Sample(gIBLSampler, float2(NdotV, roughness));
        float3 specIBL = iblSpecular * (Fibl * envBRDF.x + envBRDF.y);
        float3 kDibl   = (1.0 - Fibl) * (1.0 - metalness);
        float3 diffIBL = kDibl * baseColor.rgb * iblDiffuse;

        iblDiffuseTerm  = diffIBL * ao * skyIBLDiffuseScale;
        iblSpecularTerm = specIBL * ao * iblStrength;
    }
    // Linear ambient floor — body contribution (absorbed by the glass volume).
    float3 flatAmbient = baseColor.rgb * ambient * ao;

    // Emissive — sits with surface terms (it is *emitted* by the surface, not
    // absorbed by the body, so it must remain visible even on thin glass).
    float3 emissiveTerm = float3(0, 0, 0);
#if HAS_EMISSIVE
    {
        float4 emissiveParam = mat.emissiveColor; // .rgb = color, .w = strength
        float3 emissiveVal   = emissiveParam.rgb * emissiveParam.w;
        int texEmissive = mat.textureHandleIds[MAT_TEX_EMISSIVEMAP];
        if (texEmissive >= 0)
            emissiveVal *= g_AllTextures[texEmissive].Sample(g_LinearWrap, i.uv).rgb;
        emissiveTerm = emissiveVal;
    }
#endif

    // ---- Output ------------------------------------------------------------
    //
    // Forward-transparent PBR splits the lit colour into two semantic groups
    // because they have different transparency physics:
    //
    //   bodyColor  — Lambertian diffuse + ambient + IBL diffuse. These come
    //                from light passing INTO the medium; for a translucent
    //                body they are absorbed in proportion to opacity, so they
    //                must be modulated by surfaceAlpha.
    //
    //   surfColor  — direct specular + IBL specular (incl. probes) + emissive.
    //                These reflect / emit AT the surface. For glass the
    //                Fresnel highlight, sky reflection, and any stylised
    //                emissive must remain visible even when the body is
    //                near-fully transparent — they are added at FULL intensity
    //                regardless of surfaceAlpha (premultiplied semantics).
    //
    // Output is premultiplied:  rgb_premult = body*alpha + surf
    //                           alpha       = surfaceAlpha
    //
    // For PREMULTIPLIED_BLEND PSO (ONE / INV_SRC_ALPHA) we return that pair as
    // is; for the default ALPHA_BLEND PSO (SRC_ALPHA / INV_SRC_ALPHA) we have
    // to pre-divide rgb by alpha because the GPU is going to multiply it back.
    //
    // An earlier revision tried to "lit-alpha boost" (raise effAlpha by surfLuma)
    // so the highlight wouldn't get crushed. With HDR direct lighting that
    // boost saturated to 1.0 across the whole sun-lit face — effAlpha=1 makes
    // the surface fully opaque and erases the scene behind, which the user
    // saw as the lit face turning solid white. Removed: surfColor alone
    // already provides "highlight at full intensity" because it is added on
    // top of  dest*(1 - surfaceAlpha)  by the premultiplied math, no alpha
    // trick required.
    float3 bodyColor = directDiffuse + iblDiffuseTerm + flatAmbient;
    float3 surfColor = directSpecular + iblSpecularTerm + emissiveTerm;

#if ADDITIVE_BLEND
    // Additive (ONE / ONE) — used for glow / fire / VFX. Whole rgb adds.
    return float4(bodyColor + surfColor, surfaceAlpha);
#elif MULTIPLY_BLEND
    // Multiply (DEST_COLOR / INV_SRC_ALPHA) — tints destination by source.
    // Glass-split semantics don't apply; pass the combined colour through.
    return float4(bodyColor + surfColor, surfaceAlpha);
#else
    // ALPHA_BLEND or PREMULTIPLIED_BLEND — premultiplied glass.
    float3 premultRgb = bodyColor * surfaceAlpha + surfColor;
    #if PREMULTIPLIED_BLEND
        // PSO blend = ONE / INV_SRC_ALPHA — output is already premultiplied.
        return float4(premultRgb, surfaceAlpha);
    #else
        // PSO blend = SRC_ALPHA / INV_SRC_ALPHA — GPU multiplies our rgb by
        // surfaceAlpha. Pre-divide so the on-screen result equals premultRgb.
        // max(alpha, 1e-3) bounds the divide for the fully-transparent case
        // (alpha=0 would otherwise NaN — the resulting rgb is unused because
        // the GPU multiplies by alpha=0, but HLSL still evaluates the divide).
        return float4(premultRgb / max(surfaceAlpha, 1e-3), surfaceAlpha);
    #endif
#endif
}

