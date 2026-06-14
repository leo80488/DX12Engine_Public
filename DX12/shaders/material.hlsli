#ifndef MATERIAL_HLSLI
#define MATERIAL_HLSLI

// material.hlsli — GPU-side material data layout.
//
// Mirrors C++ Resource::MaterialGPUData in include/Resource/SystemHandles.h.
// As of M3-Day-1 (2026-04) the engine PBR + NPR fields are NAMED struct
// members (not packed into a generic params[N] array). DXC reflection on
// StructuredBuffer<MaterialGPUData> now surfaces them by name, which lets
// the editor inspector / future ShaderLab UI iterate them generically the
// same way it already iterates the customParams[] map.
//
// Each field block below is sized to a multiple of 16 bytes so HLSL natural
// alignment + #pragma pack(1) on the C++ side land at the same offsets.
// Pad fields exist purely to honour those boundaries — keep them when
// reordering.

// Shader type values (must match C++ MaterialComponent::SHADERTYPE)
#define SHADER_PBR        0
#define SHADER_UNLIT      2
#define SHADER_NPR_RAMP   4
#define SHADER_NPR_COLOR  5

// Texture handle indices (material.textureHandleIds[N])
#define MAT_TEX_EMISSIVEMAP 6
#define MAT_TEX_RAMPMAP     7

struct MaterialGPUData
{
    // ---- PBR scalars (16B per block) ----
    float roughnessMin;      // remap: lerp(min, max, surfaceMap.r)
    float roughnessMax;
    float metalnessMin;
    float metalnessMax;

    float reflectance;       // F0 = 0.16 * reflectance^2 for dielectrics
    float normalStrength;
    float saturation;
    float alphaRef;

    // ---- PBR colors (linear-space, populated from MaterialComponent's cache) ----
    float4 baseColor;        // .rgb tint, .a = master alpha
    float4 specularColor;    // .rgb specular F0 override (specular-glossiness path)
    float4 emissiveColor;    // .rgb tint, .a = strength

    // ---- NPR (only meaningful when shaderType == SHADER_NPR_*) ----
    uint   shaderType;       // see SHADER_* enum above
    float  nprMinBrightness; // also acts as "is NPR" flag in normal.w encoding
    float  nprShadowThreshold;
    float  nprShadowSmooth;

    float  nprRimPower;
    float  nprRimStrength;
    float  nprRampBlend;     // 0 = baseColor only, 1 = full ramp
    float  nprBrightnessClamp;

    // float3 + float = 16B, no internal padding under HLSL natural alignment
    float3 nprDiffuseRampColor;
    float  nprMaxBrightness; // absolute HDR ceiling

    float3 nprShadowRampColor;
    float  _padNprShadow;

    float  nprMidWeight;     // skin layer blend weights
    float  nprVeinWeight;
    float  nprSSSWeight;
    float  _padNprSss;

    // ---- Texture bindings + counts ----
    int    textureHandleIds[8];   // bindless indices; -1 if unloaded (32B)
    uint   paramCount;            // legacy — kept so old shaders compile if any
    uint   textureCount;
    uint   materialFlags;         // MAT_FLAG_* bitmask
    uint   _padFlags;

    // ---- Custom-shader scratch (M3.1 capacity: 32 params, 8 textures) ----
    float4 customParams[32];      // 512B
    int    customTextureIds[8];   //  32B
    uint   customParamCount;
    uint   customTextureCount;
    uint   customShadingModel;    // 0=Standard 1=Unlit 2=ClearCoat 3=Subsurface 4=Anisotropic
    uint   _pad2;
};

// Per-material GPU flag bitmask (MaterialGPUData.materialFlags).
// C++ source: Resource::MaterialGPUData is populated by Renderer::WriteMatSlot
// from MaterialComponent::FLAGS in Components.h. Keep in sync with that enum.
#define MAT_FLAG_EXCLUDE_FROM_SSAO       (1u << 0)   // pixel is skipped by XeGTAO
#define MAT_FLAG_DISABLE_RECEIVE_SHADOW  (1u << 1)   // pixel ignores CSM (always lit)
#define MAT_FLAG_USE_VERTEXCOLOR         (1u << 2)   // multiply baseColor by per-vertex color
                                                     // (MaterialComponent::USE_VERTEXCOLORS)

// Phase E shading-model constants — must match C++ enum ShadingModel.
#define SHADING_MODEL_STANDARD     0u
#define SHADING_MODEL_UNLIT        1u
#define SHADING_MODEL_CLEARCOAT    2u
#define SHADING_MODEL_SUBSURFACE   3u
#define SHADING_MODEL_ANISOTROPIC  4u

#endif // MATERIAL_HLSLI
