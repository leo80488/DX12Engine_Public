#pragma once

// GameScene — actual gameplay state.
//
// Init responsibilities:
//   - Reset the World (Clear) so we don't inherit Title's leftovers
//   - If game.json specifies "startup_scene", LoadScene it
//   - Otherwise spawn a minimal default scene (camera + light + skybox + cube)
//
// Update responsibilities:
//   - Listen for game-over conditions and transition to EndScene
//   - (placeholder: Q key triggers end for testing)
//
// Engine systems (script/physics/animation) tick in App on the same World;
// no per-frame work here unless the game needs scripted level events.

#include "Scene/IGameMode.h"

#include <vector>

class GameScene : public IGameMode
{
public:
    const char* GetName() const override { return "GameScene"; }

    void Init    (GameModeContext* ctx) override;
    void Update  (float dt) override;
    void Shutdown() override;

private:
    std::vector<Entity> m_spawnedEntities;
    bool                m_loadedFromManifest = false;
};
