#pragma once

// MaterialReflectionSync — bridges ShaderReflect::Reflection (the shader's
// declared resource bindings + cbuffer variables) to MaterialComponent's
// per-name customTextures / customParams stores.
//
// Called by the renderer once after a material's custom shader is resolved
// (or every frame — it's cheap; missing keys get added with defaults, present
// keys are left alone). Stale entries (no longer in the shader) are kept
// silently so a transient compile failure doesn't drop the user's tweaks.
//
// Naming convention for "is this a texture binding?":
//   ShaderReflect::ResourceType::Texture   → goes into customTextures
// Naming convention for "is this a custom param?":
//   ANY ShaderReflect::CBufferLayout that's NOT one of the engine-reserved
//   buffer names (PerView, MaterialBuffer, MeshDescriptors, …) — those are
//   bound globally by the GBuffer pass and must not show up as user knobs.
//
// Reserved names list lives in the .cpp; add new engine cbuffers there as
// they are introduced.

#include "ECS/Components.h"
#include "Graphics/ShaderReflection.h"

namespace MaterialReflectionSync
{
    // Adds defaults for any reflected texture binding / cbuffer var not yet
    // in the material's maps. Returns the number of new entries added so the
    // caller can decide whether to flag the material dirty.
    std::size_t Sync(MaterialComponent& m, const ShaderReflect::Reflection& r);

    // True when the cbuffer is bound globally by the engine (PerView, etc.)
    // and therefore should NOT surface as a per-material parameter.
    bool IsReservedCBuffer(const char* name);

    // True when the texture binding is populated by the engine (per-draw
    // fallback slots, bindless array). Custom-shader authors reusing these
    // names get the engine-bound values transparently; the inspector must
    // NOT offer them as user-assignable slots.
    bool IsReservedTexture(const char* name);
}
