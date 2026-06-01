#pragma once

// TestScene — default level: a thin gameplay-only IGameMode.
//
// Post-refactor (Option B):
//   - World, ScriptSystem, PhysicsSystem, CameraSystem all live in App.
//   - This scene's Init populates the App-owned World with default props
//     (camera, directional light, IBL skybox, demo cube). Update is empty —
//     gameplay logic for the default level lives in Lua scripts.
//   - Shutdown clears the entities this scene spawned so popping the scene
//     leaves the World empty for the next push.

#include "Scene/IGameMode.h"

#include <vector>

class TestScene : public IGameMode
{
public:
    const char* GetName() const override { return "TestScene"; }

    void Init    (GameModeContext* ctx) override;
    void Update  (float dt) override;
    void Shutdown() override;

private:
    // Track entities spawned by this scene so Shutdown can remove them
    // without touching anything created later by gameplay scripts.
    std::vector<Entity> m_spawnedEntities;
};
