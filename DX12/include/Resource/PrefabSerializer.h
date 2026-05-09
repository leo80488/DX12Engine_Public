#pragma once

// PrefabSerializer — save/load an ECS entity tree to/from a .ipfb binary file.
//
// .ipfb blob layout:
//   [AssetHeader (24 B)] [PrefabMetadata (16 B)] [line-based text payload (null-terminated)]
//
// Text payload format (generic component serialization via ComponentSerializerRegistry):
//   N idx=<i> parent=<j|-1> tx=.. ty=.. tz=.. qx=.. qy=.. qz=.. qw=.. sx=.. sy=.. sz=..
//     name=<percent-encoded-name>
//   <2-space indented component lines for that entity, one per registered component>
//     Tag: key=val key=val ...
//
// Example:
//   N idx=0 parent=-1 tx=0 ty=0 tz=0 qx=0 qy=0 qz=0 qw=1 sx=1 sy=1 sz=1 name=PointLight
//     LightData: type=1 radius=10.0000 intensity=1.0000 color=1_1_1 dir=0_-1_0 spotAngle=0.5236
//     Billboard: mode=2 worldSize=1.0000
//     MeshRef: path=asset/mesh.imsh
//     MaterialRef: path=asset/mat.imat
//
// Component types: INLINE (data in file), REF (path → ResourceSystem loads), DERIVED (runtime).
// Names and paths use percent-encoding for space (%20) and literal % (%25).

#include "ECS/ECS.h"
#include <string>

class Renderer;

namespace Resource
{
    class AssetManager;
    class AnimationClipSystem;

    // Serialize the entity subtree rooted at 'root' (including all descendants)
    // into a .ipfb file at 'path'. Returns true on success.
    // Saves skinned mesh data (SceneSourcePath, AnimationSourcePath) if present.
    bool SavePrefab(Entity root, World& world, const std::string& path);

    // Deserialize a .ipfb file and instantiate the entity tree into 'world'.
    // Mesh GPU buffers are loaded and deduplicated via assetMgr (path-cached).
    // If renderer and animClipSys are non-null, skinned mesh + animation will be
    // reconstructed from the stored .iscn and .ianim paths.
    // Returns the root entity on success, NullEntity on failure.
    Entity LoadPrefab(const std::string& path, World& world, AssetManager& assetMgr,
                      Renderer* renderer = nullptr,
                      AnimationClipSystem* animClipSys = nullptr);
}
