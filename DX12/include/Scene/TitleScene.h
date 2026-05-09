#pragma once

// TitleScene — game state-machine entry point.
//
// Responsibility:
//   - Show a title state (currently log-only; replace with logo entity / UI later)
//   - Wait for SPACE key, then transition to GameScene
//
// Wiring contract:
//   App pushes this as the initial scene in Game builds. Editor builds use
//   TestScene instead so the editor's level-editing flow stays direct.

#include "Scene/IScene.h"

#include <vector>

class TitleScene : public IScene
{
public:
    const char* GetName() const override { return "TitleScene"; }

    void Init    (SceneContext* ctx) override;
    void Update  (float dt) override;
    void Shutdown() override;

private:
    std::vector<Entity> m_spawnedEntities;
};
