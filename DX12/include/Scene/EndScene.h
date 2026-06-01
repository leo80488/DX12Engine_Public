#pragma once

// EndScene — game-over / victory state.
//
// Currently a thin demo: black-ish background, ENTER returns to TitleScene.
// Replace the entity setup with real result UI (final score, leaderboard,
// continue prompt) when you have a UI layer.

#include "Scene/IGameMode.h"

#include <vector>

class EndScene : public IGameMode
{
public:
    const char* GetName() const override { return "EndScene"; }

    void Init    (GameModeContext* ctx) override;
    void Update  (float dt) override;
    void Shutdown() override;

private:
    std::vector<Entity> m_spawnedEntities;
};
