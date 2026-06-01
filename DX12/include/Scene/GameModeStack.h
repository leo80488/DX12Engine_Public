#pragma once

// GameModeStack — simple stack of IGameMode instances.
//
// The top of the stack is the active game mode. PushMode calls Init;
// PopMode calls Shutdown. Update delegates to the top mode's gameplay
// hook. Rendering is NOT a per-mode concern post-refactor — App drives
// the renderer uniformly using the shared App-owned World.

#include "Scene/IGameMode.h"

#include <memory>
#include <vector>

class GameModeStack
{
public:
    // Push a new mode and call its Init immediately.
    void PushMode(std::unique_ptr<IGameMode> mode, GameModeContext& ctx);

    // Pop the top mode and call its Shutdown.
    void PopMode();

    // Delegate to the active (top) mode, or no-op if stack is empty.
    void Update(float dt);

    // Same dispatch as Update but for the active mode's ImGui hook.
    // App calls this from inside the BeginImGuiFrame / EndImGuiFrame block.
    void OnUIRender(GameModeContext& ctx);

    IGameMode* GetActive() const;
    bool       IsEmpty()   const { return m_stack.empty(); }

private:
    std::vector<std::unique_ptr<IGameMode>> m_stack;
};
