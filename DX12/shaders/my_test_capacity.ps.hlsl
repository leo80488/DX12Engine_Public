// my_test_capacity.ps.hlsl — capacity smoke test for the M3.1 reflection-driven
// material refactor. Declares 14 named scalar/vector params and 6 named
// textures, exercising the bumped customParams (8→32) / customTextureIds (4→8)
// limits. Drag-drop onto any material's Custom Shader slot to surface every
// param as a slider in the Inspector and confirm the panel scales past the
// original 8-param ceiling.
//
// CustomParams declaration order (= GPU customParams[] index order):
//   [0]  TintColor            float4 — multiplied into albedo
//   [1]  TintStrength         float  — 0..1 tint mix
//   [2]  RoughnessOverride    float  — 0..1 forced roughness
//   [3]  MetalnessOverride    float  — 0..1 forced metalness
//   [4]  EmissiveColor        float4 — added on top of albedo
//   [5]  EmissiveIntensity    float  — multiplier for EmissiveColor
//   [6]  RimColor             float4 — fresnel rim tint
//   [7]  RimPower             float  — rim falloff exponent
//   [8]  NormalScale          float  — normal-map intensity
//   [9]  ParallaxDepth        float  — parallax displacement scalar (placeholder)
//   [10] AlphaScale           float  — final alpha multiplier
//   [11] UVScale              float2 — uv multiplier
//   [12] UVOffset             float2 — uv offset
//   [13] DetailContribution   float  — placeholder weight
//
// CustomTextures declaration order (= GPU customTextureIds[] index order):
//   [0]  AlbedoMap     — base colour, replaces engine baseColor sample if assigned
//   [1]  NormalMap     — tangent-space, scaled by NormalScale
//   [2]  RoughnessMap  — single channel
//   [3]  MetalnessMap  — single channel
//   [4]  EmissiveMap   — multiplies EmissiveColor
//   [5]  DetailMap     — added to albedo at DetailContribution

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
Texture2D                         g_AllTextures[]: register(t0, space2);
SamplerState                      g_LinearWrap    : register(s0, space0);

// 14 named params — Inspector should auto-generate widgets for all of them.
cbuffer CapacityParams : register(b8, space0)
{
    float4 TintColor;
    float  TintStrength;
    float  RoughnessOverride;
    float  MetalnessOverride;
    float  _pad0;
    float4 EmissiveColor;
    float  EmissiveIntensity;
    float  RimPower;
    float  NormalScale;
    float  ParallaxDepth;
    float4 RimColor;
    float  AlphaScale;
    float  DetailContribution;
    float2 UVScale;
    float2 UVOffset;
    float2 _pad1;
};

// 6 named textures — Inspector should show 6 drag-drop slots.
Texture2D AlbedoMap    : register(t0, space3);
Texture2D NormalMap    : register(t1, space3);
Texture2D RoughnessMap : register(t2, space3);
Texture2D MetalnessMap : register(t3, space3);
Texture2D EmissiveMap  : register(t4, space3);
Texture2D DetailMap    : register(t5, space3);

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

struct GOut
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 surface  : SV_TARGET2;
    float2 velocity : SV_TARGET3;
    float4 extra    : SV_TARGET4;
    float4 sceneCol : SV_TARGET5;   // HdrSceneColor: emissive seed (additive lighting)
};

GOut main(PSIn i)
{
    MaterialGPUData mat = g_Materials[materialIndex];

    const float2 uv = i.uv * UVScale + UVOffset;

    // Albedo: prefer named AlbedoMap, fall back to engine BaseColor slot.
    float4 baseCol = AlbedoMap.Sample(g_LinearWrap, uv);
    if (all(baseCol.rgb == 0.0)) baseCol = g_BaseColor.Sample(g_LinearWrap, uv);

    float3 albedoOut = lerp(baseCol.rgb, baseCol.rgb * TintColor.rgb, TintStrength);

    // Detail map adds back into albedo at user-controlled weight.
    float3 detail = DetailMap.Sample(g_LinearWrap, uv).rgb;
    albedoOut += detail * DetailContribution;

    // Emissive: tint × intensity × map. Written to RT5 (HdrSceneColor) so it
    // bypasses the BRDF entirely — see GBuffer.ps for the full rationale.
    float3 emis = EmissiveMap.Sample(g_LinearWrap, uv).rgb
                  * EmissiveColor.rgb * EmissiveIntensity;

    // Surface — overrides win over map samples when slider is non-default.
    float roughness = (RoughnessOverride > 0.0)
        ? RoughnessOverride
        : RoughnessMap.Sample(g_LinearWrap, uv).r;
    float metalness = (MetalnessOverride > 0.0)
        ? MetalnessOverride
        : MetalnessMap.Sample(g_LinearWrap, uv).r;
    float ao        = 1.0;
    float reflect_  = 0.5;

    // Normal: tangent-space if NormalMap assigned, else geometry normal.
    float3 sampledNormal = NormalMap.Sample(g_LinearWrap, uv).rgb * 2.0 - 1.0;
    sampledNormal.xy *= NormalScale;
    float3 N = (length(sampledNormal) > 0.0)
        ? normalize(i.wn + sampledNormal.x * i.wt + sampledNormal.y * i.wbt)
        : normalize(i.wn);

    // Reference everything reflection should pick up, even if the math is
    // dead, so DXC keeps the cbuffer var alive and the Inspector sees it.
    float keepAlive = (RimColor.r + RimPower + ParallaxDepth + AlphaScale) * 0.0;

    float2 curNDC  = i.curClip.xy  / i.curClip.w;
    float2 prevNDC = i.prevClip.xy / i.prevClip.w;

    const bool excludeSSAO = (mat.materialFlags & MAT_FLAG_EXCLUDE_FROM_SSAO) != 0;
    const float matIdxEnc  = excludeSSAO ? -(float(materialIndex) + 1.0)
                                         :  (float(materialIndex) + 1.0);

    GOut o;
    o.albedo   = float4(albedoOut + keepAlive.xxx, baseCol.a * AlphaScale);
    o.normal   = float4(N * 0.5 + 0.5, matIdxEnc);
    o.surface  = float4(roughness, metalness, ao, reflect_);
    o.velocity = curNDC - prevNDC;   // raw NDC delta, matches GBuffer.ps
    o.extra    = float4(0, 0, 0, 0);
    o.sceneCol = float4(emis, 1.0);
    return o;
}

