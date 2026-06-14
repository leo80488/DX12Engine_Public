#pragma once

// SceneDefaults — the ONE generic "empty default world" used as a safety net
// when a data scene's .iscene file is missing or fails to load.
//
// This replaces the three near-identical hardcoded SpawnFallback/SpawnDefaults
// functions that used to live inside TitleScene/GameScene/EndScene. It is
// deliberately scene-AGNOSTIC: a camera, a directional light, and an IBL
// skybox so the viewport isn't pitch black. Per-scene identity now comes from
// the .iscene file (content) + the Lua scene script (behaviour) — NOT from C++.

#include "ECS/ECS.h"

namespace Scene
{
    // Spawn camera + directional light + IBL skybox into `world`. Assumes the
    // caller already cleared the world. Used by DataScene when the requested
    // .iscene cannot be loaded.
    void SpawnDefaultWorld(World& world);
}
