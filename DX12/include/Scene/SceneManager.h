#pragma once

// SceneManager — simple scene stack.
//
// The top of the stack is the active scene. PushScene calls Init;
// PopScene calls Shutdown. Update delegates to the top scene's gameplay hook.
// Rendering is NOT a per-scene concern post-refactor — App drives the
// renderer uniformly using the shared App-owned World.

#include "Scene/IScene.h"

#include <memory>
#include <vector>

class SceneManager
{
public:
    // Push a new scene and call its Init immediately.
    void PushScene(std::unique_ptr<IScene> scene, SceneContext& ctx);

    // Pop the top scene and call its Shutdown.
    void PopScene();

    // Delegate to the active (top) scene, or no-op if stack is empty.
    void Update(float dt);

    // Same dispatch as Update but for the active scene's ImGui hook.
    // App calls this from inside the BeginImGuiFrame / EndImGuiFrame block.
    void OnUIRender(SceneContext& ctx);

    IScene* GetActiveScene() const;
    bool    IsEmpty()        const { return m_stack.empty(); }

private:
    std::vector<std::unique_ptr<IScene>> m_stack;
};
