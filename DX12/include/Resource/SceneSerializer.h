#pragma once

// SceneSerializer — save/load the entire ECS World to/from a .iscene binary file.
//
// Naming note (post-rename 2026-05-25): the disk format is a *scene* asset
// (a serialised description of what lives in a level); the runtime container
// is the `World`. So `SaveScene(World&, path)` reads as "save the live world
// to this scene asset" and `LoadScene(path, World&)` as "load this scene
// asset into the live world" — the asymmetry is intentional.
//
// .iscene blob layout:
//   [AssetHeader (24 B)] [SceneMetadata (16 B)] [line-based text payload (null-terminated)]
//
// Text payload format (generic component serialization via ComponentSerializerRegistry):
//   W name=<scene-name>
//   N idx=<i> parent=<j|-1> tx=.. ty=.. tz=.. qx=.. qy=.. qz=.. qw=.. sx=.. sy=.. sz=..
//     name=<percent-encoded-name>
//   <2-space indented component lines, same as .ipfb>
//     Tag: key=val key=val ...
//
// Skinned entities (SceneRef component) have their children regenerated from
// the .iscn file on load — child entities are NOT stored in the .iscene file.
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

    // Serialize the entire World to a .iscene file.
    // sceneName is stored as metadata (displayed in editor title bar, etc.)
    // postProcessConfigPath, if non-empty, is stored on the W line and causes
    // LoadScene to automatically load + apply that .ippc after world restore.
    // sceneScript, if non-empty, is the path to a Lua "scene script" — a
    // singleton bound to the whole scene (NOT an entity) with lifecycle hooks
    // OnSceneEnter / OnSceneUpdate(dt) / OnSceneExit. It drives the data-driven
    // scene flow (what used to be the hardcoded TitleScene/GameScene/EndScene
    // Update logic). Stored on the W line and surfaced via LoadScene's
    // outSceneScript so the runtime SceneManager can activate it on load.
    bool SaveScene(World& world, const std::string& path,
                   const std::string& sceneName = "Untitled",
                   const std::string& postProcessConfigPath = "",
                   const std::string& navMeshPath = "",
                   const std::string& sceneScript = "");

    // Deserialize a .iscene file, CLEAR the world, then rebuild all entities.
    // If the scene referenced a .ippc and a Renderer is supplied, the post-
    // processing config is loaded and applied in the same call.
    // Returns true on success. outPostProcessConfigPath (optional) receives
    // the .ippc path that was stored in the file, whether or not it was
    // successfully applied. outNavMeshPath receives the .inav path stored on
    // the W line — caller is responsible for feeding it to NavMeshSystem::Load.
    bool LoadScene(const std::string& path, World& world, AssetManager& assetMgr,
                   Renderer* renderer = nullptr,
                   AnimationClipSystem* animClipSys = nullptr,
                   std::string* outSceneName = nullptr,
                   std::string* outPostProcessConfigPath = nullptr,
                   std::string* outNavMeshPath = nullptr,
                   std::string* outSceneScript = nullptr);
}
