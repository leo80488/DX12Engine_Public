#pragma once

// DataScene — the ONE generic runtime game-mode that hosts a data-driven scene.
//
// It replaces the family of hardcoded IGameMode classes (TitleScene, GameScene,
// EndScene), each of which baked a .iscene path + fallback content + transition
// logic into C++. A DataScene carries only an opaque scene identifier (a
// registry name or a raw .iscene path); everything else is data:
//   - Init  : delegate the World reload to SceneManager::ActivateScene(id),
//             which also activates the scene's Lua scene script.
//   - Update: nothing — the scene script's OnSceneUpdate ticks inside
//             ScriptSystem, and engine systems (script/physics/anim) tick in
//             App on the shared World regardless of which mode is active.
//   - Shutdown: nothing to tear down here. Scene-to-scene transitions exit the
//             old scene script via ScriptSystem's deferred swap when the next
//             DataScene activates; app-quit exit is handled by ScriptSystem.
//
// SceneManager constructs DataScenes for transitions; App pushes the first one
// for the startup scene in Game builds.

#include "Scene/IGameMode.h"

#include <string>

class DataScene : public IGameMode
{
public:
    explicit DataScene(std::string sceneId) : m_sceneId(std::move(sceneId)) {}

    const char* GetName() const override { return m_sceneId.c_str(); }

    void Init    (GameModeContext* ctx) override;
    void Update  (float dt) override;
    void Shutdown() override;

private:
    std::string m_sceneId;  // registry name OR raw .iscene path
};
