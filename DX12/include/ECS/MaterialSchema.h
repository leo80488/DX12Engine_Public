#pragma once

// MaterialSchema — declarative description of MaterialComponent's editable
// fields. Every consumer that needs to "iterate the material's named params"
// (Inspector widget loop, .imat serialization, future ShaderLab UI) reads
// from kMaterialPBRSchema instead of hard-coding the X-macro dispatch.
//
// The MATERIAL_PROPS X-macro in Components.h still drives the C++ field
// declarations themselves, but everything ELSE (UI / serialization) is now
// schema-driven — same pattern that custom-shader params already use.
//
// To add a new editable field:
//   1. Add the C++ member to MaterialComponent (or to MATERIAL_PROPS for the
//      core PBR set).
//   2. Append a row to kMaterialPBRSchema with name / label / kind / offset /
//      slider range / shader-type visibility mask. The Inspector picks it up
//      automatically on next build.

#include "ECS/Components.h"

#include <cstddef>
#include <cstdint>

namespace MaterialSchema
{
    // Bitmask over MaterialComponent::SHADERTYPE values — controls which
    // shader-type contexts surface a given field. ShaderType N is bit (1<<N).
    constexpr uint32_t SM_PBR       = 1u << MaterialComponent::SHADERTYPE_PBR;
    constexpr uint32_t SM_PARALLAX  = 1u << MaterialComponent::SHADERTYPE_PBR_PARALLAXOCCLUSIONMAPPING;
    constexpr uint32_t SM_UNLIT     = 1u << MaterialComponent::SHADERTYPE_UNLIT;
    constexpr uint32_t SM_CLEARCOAT = 1u << MaterialComponent::SHADERTYPE_PBR_CLEARCOAT;
    constexpr uint32_t SM_NPR_RAMP  = 1u << MaterialComponent::SHADERTYPE_NPR_RAMP;
    constexpr uint32_t SM_NPR_COLOR = 1u << MaterialComponent::SHADERTYPE_NPR_COLOR;

    constexpr uint32_t SM_ANY_PBR = SM_PBR | SM_PARALLAX | SM_CLEARCOAT;
    constexpr uint32_t SM_ANY_NPR = SM_NPR_RAMP | SM_NPR_COLOR;
    constexpr uint32_t SM_ANY     = SM_ANY_PBR | SM_UNLIT | SM_ANY_NPR;

    enum class Kind : uint8_t
    {
        Float,           // single float — DragFloat / SliderFloat
        Color3,          // first 3 floats of XMFLOAT4 — ColorEdit3 (no alpha)
        Color4,          // full XMFLOAT4 — ColorEdit4
        FloatComponent,  // one component of an XMFLOAT4 (e.g. .w as strength) — SliderFloat
    };

    struct FieldDesc
    {
        const char* name;        // stable identifier, used as .imat key + override key
        const char* label;       // human-readable Inspector label
        const char* group;       // optional grouping header in Inspector; nullptr = ungrouped
        Kind        kind;
        std::size_t offset;      // offsetof(MaterialComponent, member)
        uint8_t     component;   // for FloatComponent: 0=x 1=y 2=z 3=w. Ignored otherwise.
        float       min;
        float       max;
        uint32_t    visibleMask; // bitmask of SM_* — field shown only when (mask & (1<<shaderType))
    };

    // Inspector + serializer both walk this in declaration order. Add new
    // fields at the end of their group; reordering existing rows can change
    // .imat round-trip ordering (cosmetic only — values are name-keyed).
    inline constexpr FieldDesc kMaterialPBRSchema[] =
    {
        // ---- Universal "Base Color" — surfaced for every shader type that
        //      reads it. PBR/NPR/Unlit all use baseColor as the albedo input.
        { "baseColor",         "Base Color",       "Color",
          Kind::Color4,
          offsetof(MaterialComponent, baseColor),         0,
          0.f, 1.f,
          SM_ANY_PBR | SM_ANY_NPR | SM_UNLIT },

        // ---- PBR scalars (default + parallax + clearcoat) ----
        { "roughnessMin",      "Roughness Min",    "PBR",
          Kind::Float,
          offsetof(MaterialComponent, roughnessMin),      0,
          0.f, 1.f,
          SM_ANY_PBR },
        { "roughnessMax",      "Roughness Max",    "PBR",
          Kind::Float,
          offsetof(MaterialComponent, roughnessMax),      0,
          0.f, 1.f,
          SM_ANY_PBR },
        { "metalnessMin",      "Metalness Min",    "PBR",
          Kind::Float,
          offsetof(MaterialComponent, metalnessMin),      0,
          0.f, 1.f,
          SM_ANY_PBR },
        { "metalnessMax",      "Metalness Max",    "PBR",
          Kind::Float,
          offsetof(MaterialComponent, metalnessMax),      0,
          0.f, 1.f,
          SM_ANY_PBR },
        { "reflectance",       "Reflectance",      "PBR",
          Kind::Float,
          offsetof(MaterialComponent, reflectance),       0,
          0.f, 1.f,
          SM_ANY_PBR },
        { "saturation",        "Saturation",       "PBR",
          Kind::Float,
          offsetof(MaterialComponent, saturation),        0,
          0.f, 2.f,
          SM_ANY_PBR },
        { "specularColor",     "Specular",         "PBR",
          Kind::Color4,
          offsetof(MaterialComponent, specularColor),     0,
          0.f, 1.f,
          SM_ANY_PBR },

        // ---- Emissive (PBR + Unlit) ----
        { "emissiveColor",     "Emissive Color",   "Emissive",
          Kind::Color3,
          offsetof(MaterialComponent, emissiveColor),     0,
          0.f, 1.f,
          SM_ANY_PBR | SM_UNLIT },
        { "emissiveStrength",  "Emissive Strength","Emissive",
          Kind::FloatComponent,
          offsetof(MaterialComponent, emissiveColor),     3,
          0.f, 16.f,
          SM_ANY_PBR | SM_UNLIT },

        // ---- Common (all engine shader types except custom) ----
        { "normalMapStrength", "Normal Strength",  "Common",
          Kind::Float,
          offsetof(MaterialComponent, normalMapStrength), 0,
          0.f, 4.f,
          SM_ANY_PBR | SM_ANY_NPR },
        { "alphaRef",          "Alpha Ref",        "Common",
          Kind::Float,
          offsetof(MaterialComponent, alphaRef),          0,
          0.f, 1.f,
          SM_ANY_PBR | SM_ANY_NPR | SM_UNLIT },

        // ---- NPR shared (NPR_RAMP + NPR_COLOR) ----
        { "nprMinBrightness",  "Min Brightness",   "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprMinBrightness),  0,
          0.f, 1.f,
          SM_ANY_NPR },
        { "nprShadowThreshold","Shadow Threshold", "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprShadowThreshold),0,
          0.f, 1.f,
          SM_ANY_NPR },
        { "nprShadowSmooth",   "Shadow Smooth",    "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprShadowSmooth),   0,
          0.001f, 0.5f,
          SM_ANY_NPR },
        { "nprRimPower",       "Rim Power",        "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprRimPower),       0,
          1.f, 16.f,
          SM_ANY_NPR },
        { "nprRimStrength",    "Rim Strength",     "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprRimStrength),    0,
          0.f, 2.f,
          SM_ANY_NPR },
        { "nprRampBlend",      "Ramp Blend",       "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprRampBlend),      0,
          0.f, 1.f,
          SM_ANY_NPR },
        { "nprBrightnessClamp","Brightness Clamp", "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprBrightnessClamp),0,
          0.f, 3.f,
          SM_ANY_NPR },
        { "nprMaxBrightness",  "Max Brightness",   "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprMaxBrightness),  0,
          0.f, 8.f,
          SM_ANY_NPR },

        // ---- NPR Ramp only (skin-layer texture-ramp blend weights) ----
        { "nprMidWeight",      "Mid Weight",       "NPR Ramp",
          Kind::Float,
          offsetof(MaterialComponent, nprMidWeight),      0,
          0.f, 1.f,
          SM_NPR_RAMP },
        { "nprVeinWeight",     "Vein Weight",      "NPR Ramp",
          Kind::Float,
          offsetof(MaterialComponent, nprVeinWeight),     0,
          0.f, 1.f,
          SM_NPR_RAMP },
        { "nprSSSWeight",      "SSS Weight",       "NPR Ramp",
          Kind::Float,
          offsetof(MaterialComponent, nprSSSWeight),      0,
          0.f, 1.f,
          SM_NPR_RAMP },

        // ---- NPR Color only (texture-less two-color ramp) ----
        { "nprDiffuseRampColor","Diffuse Ramp",    "NPR Color",
          Kind::Color3,
          offsetof(MaterialComponent, nprDiffuseRampColor),0,
          0.f, 1.f,
          SM_NPR_COLOR },
        { "nprShadowRampColor","Shadow Ramp",      "NPR Color",
          Kind::Color3,
          offsetof(MaterialComponent, nprShadowRampColor),0,
          0.f, 1.f,
          SM_NPR_COLOR },
    };

    constexpr std::size_t kFieldCount = sizeof(kMaterialPBRSchema) / sizeof(kMaterialPBRSchema[0]);

    // Pointer to the first float of @p field within @p mat. Caller knows the
    // field's Kind and reads the right number of floats from this pointer.
    inline float* FieldPtr(MaterialComponent& mat, const FieldDesc& f)
    {
        return reinterpret_cast<float*>(reinterpret_cast<char*>(&mat) + f.offset);
    }
    inline const float* FieldPtr(const MaterialComponent& mat, const FieldDesc& f)
    {
        return reinterpret_cast<const float*>(reinterpret_cast<const char*>(&mat) + f.offset);
    }
}
