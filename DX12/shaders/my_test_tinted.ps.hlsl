// my_test_tinted.ps.hlsl — Phase E end-to-end test for custom GBuffer PS.
//
// Demonstrates:
//   - Dynamic shader compilation + PSO swap (Phase B)
//   - Reflection-driven Inspector UI for textures + params (Phase C / D)
//   - Per-material packed CBV at b8 space0 (Phase E)
//
// Flow:
//   1. Declare `cbuffer MyTintParams : register(b8, space0) { ... }` —
//      the Renderer reflects this, Inspector auto-renders sliders for each
//      field, and packs user-set values into an upload-heap CBV each frame.
//   2. Declare a Texture2D without a register — reflection picks up the
//      name so Inspector shows a drag-drop field; the value lands in
//      MaterialGPUData.customTextureIds[0] (accessed through g_AllTextures).
//   3. Standard engine bindings (PushConstants / g_Materials / g_BaseColor /
//      g_AllTextures / g_LinearWrap) stay the same — reflection skips them
//      because MaterialReflectionSync::kReservedCBuffers and kReservedTextures
//      filter them out of the user-facing UI.
//
// Usage:
//   Inspector → check `Use Custom Shader`, drag this file into Shader Path.
//   Next frame the Material inspector grows a `Custom Parameters` section
//   with TintColor / Brightness sliders and a `Custom Textures` section
//   with a Detail texture field.

#include "material.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

StructuredBuffer<MaterialGPUData> g_Materials : register(t2, space0);
Texture2D<float4>                 g_BaseColor  : register(t3, space0);
Texture2D                         g_AllTextures[] : register(t0, space2);
SamplerState                      g_LinearWrap    : register(s0, space0);

// --- User-defined custom params (Phase E) ---
// Reflection picks these up; Renderer packs values here every frame based on
// what the Material inspector sliders show. Register slot b8 is the engine's
// per-material custom CBV root param (see GraphicsDX12 kCustomMatCBVSlot).
cbuffer MyTintParams : register(b8, space0)
{
    float4 TintColor;   // ColorEdit in Inspector (name ends in "Color")
    float  Brightness;  // DragFloat slider
};

// --- User-defined custom textures (Phase F) ---
// Each Texture2D at t0-t3 space3 surfaces in the Inspector as a drag-drop
// field. The Renderer copies the user-assigned texture's SRV into a
// per-material descriptor table (root slot 36) each frame.
Texture2D DetailTex : register(t0, space3);

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

// RT4 is the "extra" slot — default GBuffer clears it to zero and no one
// reads it unless the shading-model branch in LightingPass is opt-ed in.
struct GOut
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 surface  : SV_TARGET2;
    float2 velocity : SV_TARGET3;
    float4 extra    : SV_TARGET4;
    float4 sceneCol : SV_TARGET5;   // HdrSceneColor: emissive seed (this shader has none)
};

GOut main(PSIn i)
{
    MaterialGPUData mat = g_Materials[materialIndex];

    // Base color path mirrors the default GBuffer PS — falls back to the
    // per-draw descriptor if the bindless path is unset.
    int    texBase  = mat.textureHandleIds[0];
    float4 baseCol  = (texBase >= 0)
        ? g_AllTextures[texBase].Sample(g_LinearWrap, i.uv)
        : g_BaseColor.Sample(g_LinearWrap, i.uv);

    // Detail tile multiplier — sampled from the user-assigned DetailTex
    // (fallback: zero descriptor → black → no effect when unset).
    float4 detail = DetailTex.Sample(g_LinearWrap, i.uv * 4.0);
    // If DetailTex is unset (zero descriptor samples as 0), neutralise:
    if (dot(detail.rgb, 1.0.xxx) < 0.001) detail = float4(1, 1, 1, 1);

    // Apply user-set tint + brightness from our packed cbuffer.
    float3 tinted = baseCol.rgb * TintColor.rgb * detail.rgb * max(Brightness, 0.001);

    // Screen-space velocity for TAA.
    float2 curNDC  = i.curClip.xy  / i.curClip.w;
    float2 prevNDC = i.prevClip.xy / i.prevClip.w;

    // Encode SSAO-exclusion into the sign of normal.a (see GBuffer.ps.hlsl).
    const bool excludeSSAO = (mat.materialFlags & MAT_FLAG_EXCLUDE_FROM_SSAO) != 0;
    const float matIdxEnc  = excludeSSAO ? -(float(materialIndex) + 1.0)
                                         :  float(materialIndex);
    GOut o;
    o.albedo   = float4(tinted, baseCol.a);
    o.normal   = float4(normalize(i.wn) * 0.5 + 0.5, matIdxEnc);
    o.surface  = float4(0.5, 0.0, 1.0, 0.04);  // rough, non-metal, full AO, dielectric F0
    o.velocity = curNDC - prevNDC;
    o.extra    = float4(0.0, 0.0, 0.0, 0.0);
    o.sceneCol = float4(0.0, 0.0, 0.0, 1.0);
    return o;
}
