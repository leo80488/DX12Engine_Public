// my_test_sss.ps.hlsl — PBR + Subsurface Scattering test shader.
//
// Exercises the full Phase A→F stack:
//   - Custom GBuffer PS that outputs standard PBR channels
//   - Declares its own cbuffer at b8 space0 — reflection-driven Inspector UI
//   - Declares a texture at t0 space3 via the named texture table
//   - Meant to be paired with Material → ShadingModel = Subsurface so the
//     LightingPass picks up the SSS branch and adds wrap-lit scatter
//
// CustomParams layout (matches cbuffer declaration order, which drives the
// MaterialGPUData.customParams[N] slot indices that LightingPass reads):
//   customParams[0] = SubsurfaceColor.rgb   (+ alpha unused)
//   customParams[1].x = WrapAmount
//   customParams[2].x = ScatterStrength
//
// CustomTextureIds layout (same rule, but for Texture2D declarations in
// source order — see GraphicsDX12 kCustomMatTexSlot):
//   customTextureIds[0] = ThicknessTex handle_id (also accessible via
//                         the named binding below)
//
// Usage in Inspector:
//   1. Check "Use Custom Shader"
//   2. Drag my_test_sss.ps.hlsl into Shader Path
//   3. Set Shading Model = Subsurface
//   4. Custom Parameters panel shows SubsurfaceColor / WrapAmount / ScatterStrength
//   5. Custom Textures panel shows ThicknessTex (drop a grayscale .itex)

#include "material.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

StructuredBuffer<MaterialGPUData> g_Materials    : register(t2, space0);
Texture2D<float4>                 g_BaseColor    : register(t3, space0);
Texture2D<float4>                 g_SurfaceMap   : register(t4, space0);
Texture2D<float4>                 g_NormalMap    : register(t5, space0);
Texture2D                         g_AllTextures[] : register(t0, space2);
SamplerState                      g_LinearWrap    : register(s0, space0);

// ---- Phase E: custom cbuffer — reflection surfaces these as Inspector sliders.
cbuffer SSSParams : register(b8, space0)
{
    float4 SubsurfaceColor;    // ColorEdit4 (warm red-ish for skin is canonical)
    float  WrapAmount;         // 0-1 slider — how far light wraps past the terminator
    float  ScatterStrength;    // 0-1 slider — how strongly the scatter tint mixes
    float2 _pad;
};

// ---- Phase F: named texture — reflection surfaces as drag-drop slot.
// Thickness is sampled in the GBuffer PS to modulate baseColor toward
// SubsurfaceColor in thick regions. LightingPass can't sample per-pixel
// textures from this shader, so we bake the thickness influence here.
Texture2D ThicknessTex : register(t0, space3);

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
    float4 curClip   : TEXCOORD1;
    float4 prevClip  : TEXCOORD2;
};

// RT4 is the "extra" slot. For Subsurface, we use it to pass per-pixel
// thickness to the lighting pass (so the SSS scatter amount can vary by
// where the light has to travel through the surface).
struct GOut
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 surface  : SV_TARGET2;
    float2 velocity : SV_TARGET3;
    float4 extra    : SV_TARGET4;   // .r = thickness (0..1) for SSS
    float4 sceneCol : SV_TARGET5;   // HdrSceneColor: emissive seed (this shader has none)
};

GOut main(PSIn i)
{
    MaterialGPUData mat = g_Materials[materialIndex];

    // Base color — same bindless-first pattern as the default GBuffer PS.
    int    texBase = mat.textureHandleIds[0];
    float4 baseCol = (texBase >= 0)
        ? g_AllTextures[texBase].Sample(g_LinearWrap, i.uv)
        : g_BaseColor.Sample(g_LinearWrap, i.uv);

    // PBR roughness/metalness — use material.params if a surface map is
    // assigned, otherwise fall back to material-uniform values. Simplified
    // from GBuffer.ps.hlsl.
    float roughness = saturate(lerp(0.3, 0.7, baseCol.a));
    float metalness = 0.0;      // skin/wax/etc — never metallic
    float ao        = 1.0;
    float reflect_  = 0.5;      // mid dielectric F0; feels right for skin

    // Normal — just geometry normal for this test (no tangent-space map).
    float3 N = normalize(i.wn);

    // Sample thickness; darken baseColor toward SubsurfaceColor in thick
    // spots so the test is visible even before LightingPass adds scatter.
    // Unassigned texture samples as zero → thickness=0 → no tint shift.
    float  thickness = ThicknessTex.Sample(g_LinearWrap, i.uv).r;
    float3 thickTint = lerp(float3(1, 1, 1), SubsurfaceColor.rgb,
                            saturate(thickness) * 0.5);
    float3 albedoOut = baseCol.rgb * thickTint;

    // Reference WrapAmount + ScatterStrength so D3DCompile keeps them in
    // reflection metadata (we don't consume them here — LightingPass does).
    // The multiplications resolve to zero at any reasonable slider value.
    float keepAlive = (WrapAmount + ScatterStrength) * 0.0;

    // Screen-space velocity (standard TAA input).
    float2 curNDC  = i.curClip.xy  / i.curClip.w;
    float2 prevNDC = i.prevClip.xy / i.prevClip.w;

    // Encode SSAO-exclusion into the sign of normal.a (see GBuffer.ps.hlsl).
    const bool excludeSSAO = (mat.materialFlags & MAT_FLAG_EXCLUDE_FROM_SSAO) != 0;
    const float matIdxEnc  = excludeSSAO ? -(float(materialIndex) + 1.0)
                                         :  float(materialIndex);
    GOut o;
    o.albedo   = float4(albedoOut + keepAlive, baseCol.a);
    o.normal   = float4(N * 0.5 + 0.5, matIdxEnc);
    o.surface  = float4(roughness, metalness, ao, reflect_);
    o.velocity = curNDC - prevNDC;
    // Publish thickness on the extra slot so LightingPass's SSS branch can
    // modulate the scatter contribution per-pixel. Unassigned ThicknessTex
    // samples as 0 → no scatter; a painted thickness mask drives soft
    // scatter on fleshy / fatty regions.
    o.extra    = float4(saturate(thickness), 0, 0, 0);
    o.sceneCol = float4(0, 0, 0, 1);   // SSS skin/wax has no emissive
    return o;
}
