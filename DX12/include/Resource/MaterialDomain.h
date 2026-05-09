#pragma once

// MaterialDomain — classifies how a material is executed.
//
// The engine uses this tag to decide which pipeline a material belongs to.
// Assets with different domains live in separate pools and upload to
// distinct GPU layouts — we intentionally do not union them into a single
// "MaterialGPU" to preserve each pipeline's layout optimisation:
//
//   Opaque      — traditional per-material PSO, GBuffer pass output.
//                 Current engine path, driven by MaterialComponent +
//                 Resource::MaterialAsset (see SystemHandles.h).
//
//   Transparent — forward TransparentPass, own PSO table. Currently
//                 folded into the Opaque path via MaterialComponent's
//                 BlendMode; remains here for future split-out.
//
//   Decal       — compute-based clustered system, NO PSO, bindless-driven.
//                 Driven by DecalMaterialAsset. Treated as a shared
//                 template so many DecalInstances can reference one
//                 DecalMaterialAsset (e.g. 1000 blood splats → 1 asset).
//
//   PostProcess — reserved; screen-space effects (tonemap, bloom, CAS).
//
// Design intent (see decal_system_prompt.md §12): share the Texture +
// BindlessRegistry layers, share the editor UI shell where feasible, but
// keep GPU layout and shader/PSO paths independent. This enum is the
// discriminator used by the editor to route inspector panels and by
// asset-save code to tag serialized files.

#include <cstdint>

namespace Resource
{
    enum class MaterialDomain : uint32_t
    {
        Opaque      = 0,
        Transparent = 1,
        Decal       = 2,
        PostProcess = 3,
    };
}
