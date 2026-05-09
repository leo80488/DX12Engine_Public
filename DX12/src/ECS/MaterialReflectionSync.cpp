#include "ECS/MaterialReflectionSync.h"

#include "System/Log.h"

#include <cstring>
#include <string_view>

namespace MaterialReflectionSync
{
namespace
{
    // CBuffers bound by the engine's GBuffer pass — must NEVER surface as
    // editable per-material params. Names match the HLSL `cbuffer` declarations
    // in shaders/GBuffer.ps.hlsl + shared headers.
    constexpr const char* kReservedCBuffers[] = {
        "PerView",
        "PerObject",
        "MaterialBuffer",
        "MeshDescriptors",
        "InstanceBuffer",
        "PushConstants",     // auto-filled per-draw (meshDescIdx / materialIndex / …)
        "$Globals",          // unnamed cbuffer — usually our root constants
    };

    // Texture bindings the engine binds globally in GBufferPass::BindGlobals.
    // Surfacing these as "Custom Textures" would be misleading — the values
    // come from the standard Material TextureMap slots (MATERIAL_TEXTURE_SLOTS),
    // not from the custom-shader texture list.
    constexpr const char* kReservedTextures[] = {
        "g_BaseColor",        // t3 space0, per-draw fallback
        "g_SurfaceMap",       // t4 space0, per-draw fallback
        "g_NormalMap",        // t5 space0, per-draw fallback
        "g_AllTextures",      // t0 space2, bindless array
        "g_LinearWrap",       // sampler — already filtered by type, here for doc
    };

    bool IsReservedCBufferName(std::string_view name)
    {
        for (const char* r : kReservedCBuffers)
            if (name == r) return true;
        return false;
    }

    bool IsReservedTextureName(std::string_view name)
    {
        for (const char* r : kReservedTextures)
            if (name == r) return true;
        return false;
    }

    // float-bag default by reflected scalar/vector type. Identity-ish: 0 for
    // most things, 1 for float4 alpha so a pure-write of an unset color slot
    // doesn't render fully transparent by accident.
    std::array<float, 4> DefaultForType(ShaderReflect::VarType t)
    {
        using V = ShaderReflect::VarType;
        switch (t)
        {
            case V::Float4:   return { 1.f, 1.f, 1.f, 1.f };
            case V::Float3:   return { 1.f, 1.f, 1.f, 0.f };
            case V::Float2:   return { 0.f, 0.f, 0.f, 0.f };
            case V::Float:    return { 0.f, 0.f, 0.f, 0.f };
            case V::Bool:     return { 0.f, 0.f, 0.f, 0.f };
            case V::Int: case V::Int2: case V::Int3: case V::Int4:
            case V::UInt: case V::UInt2: case V::UInt3: case V::UInt4:
                              return { 0.f, 0.f, 0.f, 0.f };
            default:          return { 0.f, 0.f, 0.f, 0.f };
        }
    }
} // namespace

std::size_t Sync(MaterialComponent& m, const ShaderReflect::Reflection& r)
{
    std::size_t added = 0;

    LOG_INFO("MaterialReflectionSync::Sync: reflection has %zu bindings, %zu cbuffers",
             r.bindings.size(), r.cbuffers.size());

    for (const ShaderReflect::ResourceBinding& b : r.bindings)
    {
        const char* typeStr = ShaderReflect::ToString(b.type);
        const bool  isTex   = (b.type == ShaderReflect::ResourceType::Texture);
        const bool  reserved = isTex && IsReservedTextureName(b.name);
        LOG_INFO("  binding [%s] '%s' bind=%u space=%u count=%u %s",
                 typeStr, b.name.c_str(), b.bindPoint, b.space, b.bindCount,
                 reserved ? "(RESERVED, skipped)" : "");

        if (b.type != ShaderReflect::ResourceType::Texture) continue;
        if (b.name.empty())              continue;
        if (IsReservedTextureName(b.name)) continue;   // engine-bound, not user-owned

        if (m.customTextures.find(b.name) == m.customTextures.end())
        {
            MaterialComponent::TextureMap t;     // empty path, runtime handles -1/0
            m.customTextures.emplace(b.name, std::move(t));
            ++added;
            LOG_INFO("    + customTextures['%s'] added", b.name.c_str());
        }
    }

    for (const ShaderReflect::CBufferLayout& cb : r.cbuffers)
    {
        const bool reserved = IsReservedCBufferName(cb.name);
        LOG_INFO("  cbuffer '%s' bind=%u space=%u size=%uB vars=%zu %s",
                 cb.name.c_str(), cb.bindPoint, cb.space, cb.sizeBytes, cb.vars.size(),
                 reserved ? "(RESERVED, skipped)" : "");
        if (reserved) continue;

        for (const ShaderReflect::CBufferVar& v : cb.vars)
        {
            if (v.name.empty()) continue;
            if (v.type == ShaderReflect::VarType::Struct) continue;       // no nested-struct UI yet
            if (v.type == ShaderReflect::VarType::Float4x4) continue;     // matrices skipped in v1
            if (v.elements > 0) continue;                                 // arrays skipped in v1

            if (m.customParams.find(v.name) == m.customParams.end())
            {
                m.customParams.emplace(v.name, DefaultForType(v.type));
                ++added;
                LOG_INFO("    + customParams['%s'] added (type=%s)",
                         v.name.c_str(), ShaderReflect::ToString(v.type));
            }
        }
    }

    LOG_INFO("MaterialReflectionSync::Sync: total entries added = %zu, "
             "customTextures.size=%zu, customParams.size=%zu",
             added, m.customTextures.size(), m.customParams.size());

    return added;
}

bool IsReservedCBuffer(const char* name)
{
    return name && IsReservedCBufferName(name);
}

bool IsReservedTexture(const char* name)
{
    return name && IsReservedTextureName(name);
}

} // namespace MaterialReflectionSync
