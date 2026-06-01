#pragma once

// WorldSerializer — save/load the entire ECS World to/from a .iworld binary file.
//
// .iworld blob layout:
//   [AssetHeader (24 B)] [WorldMetadata (16 B)] [line-based text payload (null-terminated)]
//
// Text payload format (generic component serialization via ComponentSerializerRegistry):
//   W name=<scene-name>
//   N idx=<i> parent=<j|-1> tx=.. ty=.. tz=.. qx=.. qy=.. qz=.. qw=.. sx=.. sy=.. sz=..
//     name=<percent-encoded-name>
//   <2-space indented component lines, same as .ipfb>
//     Tag: key=val key=val ...
//
// Skinned entities (SceneRef component) have their children regenerated from
// the .iscn file on load — child entities are NOT stored in the .iworld file.
//
// All root entities and their subtrees are serialized. On load, the world is
// cleared and rebuilt from scratch.

#include "ECS/ECS.h"
#include <string>

class Renderer;

namespace Resource
{
    class AssetManager;
    class AnimationClipSystem;

    // Serialize the entire World to a .iworld file.
    // sceneName is stored as metadata (displayed in editor title bar, etc.)
    // postProcessConfigPath, if non-empty, is stored on the W line and causes
    // LoadWorld to automatically load + apply that .ippc after world restore.
    bool SaveWorld(World& world, const std::string& path,
                   const std::string& sceneName = "Untitled",
                   const std::string& postProcessConfigPath = "",
                   const std::string& navMeshPath = "");

    // Deserialize a .iworld file, CLEAR the world, then rebuild all entities.
    // If the scene referenced a .ippc and a Renderer is supplied, the post-
    // processing config is loaded and applied in the same call.
    // Returns true on success. outPostProcessConfigPath (optional) receives
    // the .ippc path that was stored in the file, whether or not it was
    // successfully applied. outNavMeshPath receives the .inav path stored on
    // the W line — caller is responsible for feeding it to NavMeshSystem::Load.
    bool LoadWorld(const std::string& path, World& world, AssetManager& assetMgr,
                   Renderer* renderer = nullptr,
                   AnimationClipSystem* animClipSys = nullptr,
                   std::string* outSceneName = nullptr,
                   std::string* outPostProcessConfigPath = nullptr,
                   std::string* outNavMeshPath = nullptr);
}
