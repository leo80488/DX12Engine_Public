// BasePass.template.hlsl — per-material PS template consumed by
// MaterialTranslator. The three /*$...*/ comment markers are
// string-substituted by the C++ translator before compilation; defines
// (SHADING_MODEL_ID, BLEND_MODE_MASKED, BLEND_MODE_TRANSLUCENT, TWO_SIDED)
// come in through D3D_SHADER_MACRO.
//
// Phase 1 scope: main() returns a single float4 placeholder so the
// compile-and-reflect pipeline works end-to-end. Phase MT5 (with the real
// GBuffer RT layout + encode helpers) swaps main() to return a GBufferOut
// struct whose channels pack the MaterialInputs + ShadingModel ID + the
// per-model CustomData0 block selected by the switch below.

#include "MaterialInputs.hlsli"

// === Texture / Sampler declarations (translator-injected) ==================
SamplerState SS : register(s0);
Texture2D MyMap : register(t2);
Texture2D MyMap0 : register(t3);
Texture2D MyMap1 : register(t4);
Texture2D MyMap2 : register(t5);
Texture2D MyMap3 : register(t6);
Texture2D MyMap4 : register(t7);


// === Material cbuffer (translator-injected) ================================


// === Vertex stage output — fixed contract with the engine VS ===============
struct VSOut
{
    float4   svPos   : SV_Position;
    float3   worldPos: POSITION;
    float3   normal  : NORMAL;
    float3   tangent : TANGENT;
    float2   uv      : TEXCOORD0;
};

// === Material evaluation ===================================================
// The translator emits one `M.{field} = {expression};` line per entry in
// MaterialAsset::inputs into the eval-body marker below. Inputs not listed
// keep their InitMaterialDefaults() value, which is safe for any
// ShadingModel that treats them as optional.
//
// IMPORTANT: do not write the literal marker string in any comment above
// this point — string-replace would substitute the comment instead of the
// real marker on the line below.
MaterialInputs EvaluateMaterial(VSOut input)
{
    MaterialInputs M = InitMaterialDefaults();
    float2 uv = input.uv;

        M.BaseColor = float3(1, 1, 1);
    M.Normal = float3(1, 1, 1);
    M.Metallic = float3(1, 1, 1);
    M.Roughness = float3(1, 1, 1);
    M.AO = float3(1, 1, 1);
    M.Emissive = float3(1, 1, 1);


    return M;
}

// === Main PS ===============================================================
float4 main(VSOut input) : SV_Target
{
    MaterialInputs M = EvaluateMaterial(input);

#if defined(BLEND_MODE_MASKED)
    // Masked: Opacity acts as a binary alpha test.
    clip(M.Opacity - 0.5);
#endif

    // ---- Fixed universal outputs — these go to GBuffer in Phase MT5 -------
    // Tangent-space normal → world space (engine input provides N + T;
    // B derives from cross-product). Same convention as the engine VS
    // interpolators elsewhere in the project.
    float3 N  = normalize(input.normal);
    float3 T  = normalize(input.tangent);
    float3 B  = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);
    float3 worldNormal = normalize(mul(M.Normal, TBN));

    // ---- Per-ShadingModel CustomData selection ---------------------------
    // Phase MT5 routes this to GBufferD. For now it only influences the
    // placeholder colour below so the output visibly differs between
    // shading models during testing.
    float4 customData = float4(0, 0, 0, 0);

#if SHADING_MODEL_ID == SHADINGMODELID_PREINTEGRATED_SKIN \
 || SHADING_MODEL_ID == SHADINGMODELID_SUBSURFACE_PROFILE \
 || SHADING_MODEL_ID == SHADINGMODELID_TWO_SIDED_FOLIAGE
    customData = float4(M.SubsurfaceColor, 0.0);
#elif SHADING_MODEL_ID == SHADINGMODELID_CLEAR_COAT
    customData = float4(M.ClearCoat, M.ClearCoatRoughness, 0.0, 0.0);
#elif SHADING_MODEL_ID == SHADINGMODELID_CLOTH
    customData = float4(M.FuzzColor, M.Cloth);
#elif SHADING_MODEL_ID == SHADINGMODELID_HAIR
    customData = float4(0.0, 0.0, M.Backlit, 0.0);
#endif

    // Phase 1 placeholder: dim the base colour by AO + add emissive so the
    // output visibly depends on several MaterialInputs fields. This is NOT
    // the real shading — the deferred lighting pass replaces it entirely
    // once the render path is wired.
    (void)worldNormal;
    (void)customData;
    float3 preview = M.BaseColor * M.AO + M.Emissive;
    return float4(preview, M.Opacity);
}
