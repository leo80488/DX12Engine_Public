#pragma once

#include "Resource/ResourceHandle.h"
#include <cstdint>
#include <string>

// System-level opaque handles issued by TextureSystem, MeshSystem, MaterialSystem.
// All three reuse the same generational Handle struct; the ResourceType tag
// disambiguates the issuing system.  Invalid sentinel: Handle{} (generation == 0).
namespace Resource
{
    using TextureHandle   = Handle;
    using MeshHandle      = Handle;
    using MaterialHandle  = Handle;
    using AnimHandle      = Handle;
    using AudioHandle     = Handle;

    constexpr TextureHandle  kInvalidTextureHandle  = Handle{};
    constexpr MeshHandle     kInvalidMeshHandle     = Handle{};
    constexpr MaterialHandle kInvalidMaterialHandle = Handle{};
    constexpr AnimHandle     kInvalidAnimHandle2    = Handle{};  // note: kInvalidAnimHandle (uint32) already used in AnimationComponents.h
    constexpr AudioHandle    kInvalidAudioHandle    = Handle{};

    // -------------------------------------------------------------------------
    // MaterialAsset — CPU-side material data owned by MaterialSystem.
    // -------------------------------------------------------------------------

    constexpr uint32_t kMaxMaterialParams = 32;
    constexpr uint32_t kMaxTextureSlots   = 8;
    // Per-material scratch regions for custom-shader parameters + textures,
    // populated by the renderer from MaterialComponent::customParams /
    // customTextures when a custom GBuffer PS is active. Shaders read these
    // as `g_Materials[materialIdx].customParams[N]` / `customTextureIds[N]`;
    // the packing order matches source-declaration order in the shader's
    // cbuffer + resource bindings (see Renderer::WriteMatSlot).
    //
    // Bumped 2026-04 from (8, 4) to (32, 8) as part of the reflection-driven
    // material refactor (Phase 3 M3.1). 32 params matches the engine's own
    // params[] count; 8 textures is enough for an NPR skin shader's full
    // (albedo / normal / surface / SSS / ramp / specular / occlusion / emissive)
    // set with one slot to spare. Element size jumps ~720B → ~1120B, still
    // well under the per-StructuredBuffer-element ceiling.
    constexpr uint32_t kMaxCustomParams   = 32;  // 32 × float4 = 512B
    constexpr uint32_t kMaxCustomTextures = 8;   //  8 × int32  =  32B

    enum class MaterialParamType : uint8_t
    {
        Float,
        Float4,
        Int,
    };

    struct MaterialParam
    {
        char              name[32] = {};
        MaterialParamType type     = MaterialParamType::Float;
        uint8_t           _pad[3]  = {};
        union
        {
            float   f;
            float   f4[4];
            int32_t i;
        } data = {};
    };

    struct MaterialAsset
    {
        char          shaderName[64]                  = {};
        MaterialParam params[kMaxMaterialParams]       = {};
        TextureHandle textureSlots[kMaxTextureSlots];
        uint32_t      paramCount   = 0;
        uint32_t      textureCount = 0;
        bool          dirty        = true;

        MaterialAsset()
        {
            for (auto& s : textureSlots) s = kInvalidTextureHandle;
        }
    };

    // GPU-ready mirror of MaterialAsset. Must be byte-for-byte identical to
    // the HLSL `MaterialGPUData` in shaders/material.hlsli — the renderer
    // writes into one and the GPU reads as the other.
    //
    // M3-Day-1 (2026-04): replaced the legacy `float params[32][4]` array
    // (with MAT_IDX_* slot conventions) with explicit named PBR + NPR
    // fields. DXC reflection on the StructuredBuffer element now exposes
    // them by name, which is what drives the upcoming reflection-driven
    // editor inspector.
    //
    // Each block below is sized to 16 bytes so the HLSL side's natural
    // alignment matches this `#pragma pack(push, 1)` C++ struct exactly.
#pragma pack(push, 1)
    struct MaterialGPUData
    {
        // ---- PBR scalars (16B per block) ----
        float    roughnessMin;
        float    roughnessMax;
        float    metalnessMin;
        float    metalnessMax;

        float    reflectance;
        float    normalStrength;
        float    saturation;
        float    alphaRef;

        // ---- PBR colors (linear-space) ----
        float    baseColor[4];
        float    specularColor[4];
        float    emissiveColor[4];

        // ---- NPR fields ----
        uint32_t shaderType;
        float    nprMinBrightness;
        float    nprShadowThreshold;
        float    nprShadowSmooth;

        float    nprRimPower;
        float    nprRimStrength;
        float    nprRampBlend;
        float    nprBrightnessClamp;

        // float3+float — 12+4 = 16B contiguous on both sides
        float    nprDiffuseRampColor[3];
        float    nprMaxBrightness;

        float    nprShadowRampColor[3];
        float    _padNprShadow;

        float    nprMidWeight;
        float    nprVeinWeight;
        float    nprSSSWeight;
        float    _padNprSss;

        // ---- Texture bindings + counts ----
        int32_t  textureHandleIds[kMaxTextureSlots];
        uint32_t paramCount;            // legacy retained
        uint32_t textureCount;
        uint32_t materialFlags;         // MAT_FLAG_* bitmask (see material.hlsli)
        uint32_t _padFlags;

        // ---- Custom-shader scratch (M3.1: 32 params / 8 textures) ----
        float    customParams[kMaxCustomParams][4];
        int32_t  customTextureIds[kMaxCustomTextures];
        uint32_t customParamCount;
        uint32_t customTextureCount;
        uint32_t customShadingModel;    // 0=Standard 1=Unlit 2=ClearCoat 3=Subsurface 4=Anisotropic
        uint32_t _pad2;
    };
#pragma pack(pop)
    static_assert(sizeof(MaterialGPUData) <= 2048,
                  "MaterialGPUData too large for one StructuredBuffer element slot");
    // 10 PBR blocks × 16B + 8×4B textureHandleIds + 4×4B counts/flags
    // + 32×16B customParams + 8×4B customTextureIds + 4×4B counts/model
    // = 160 + 32 + 16 + 512 + 32 + 16 = 768 bytes
    static_assert(sizeof(MaterialGPUData) == 768,
                  "MaterialGPUData layout drifted — HLSL mirror in material.hlsli must match");
}
