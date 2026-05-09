#pragma once

// SceneInstanceLoader — instantiates a cooked .iscn asset into the ECS world.
// Reads the .iscn text, loads each sibling .imsh mesh file, uploads GPU buffers
// via MeshSystem, and builds a Parent/Children hierarchy with MaterialComponent
// on every mesh entity.
//
// If the .iscn contains a K line (skeleton file) and renderer != nullptr,
// the skeleton is deserialized and skinned mesh entities are registered via
// Renderer::RegisterSkinnedMeshFull().

#include "ECS/ECS.h"
#include <string>

namespace Resource { class AssetManager; class MeshLibrary; }
class Renderer;

class SceneInstanceLoader
{
public:
    struct LoadResult
    {
        Entity   rootEntity      { NullEntity };
        uint32_t nodeCount       { 0 };
        uint32_t meshEntityCount { 0 };
        bool     success         { false };
    };

    /** Load an .iscn file and all its sibling .imsh files into the world.
     *  Meshes are loaded and deduplicated via assetMgr (path-cached).
     *  Pass renderer != nullptr to enable skeleton/animation loading when
     *  the .iscn references a sibling .iskel file. */
    // Loads .meshlib via MeshLibrary (first-class mesh entries) and spawns
    // one entity per mesh ref with a MeshLibRef component pointing at the
    // library's entry. assetMgr is still accepted so texture/material paths
    // (via .imat files) can be resolved. renderer != nullptr enables
    // skeleton/animation loading when the .iscn references a .iskel.
    static LoadResult Load(const std::string&      iscnPath,
                           World&                  world,
                           Resource::AssetManager& assetMgr,
                           Resource::MeshLibrary&  meshLib,
                           Renderer*               renderer = nullptr);

};
