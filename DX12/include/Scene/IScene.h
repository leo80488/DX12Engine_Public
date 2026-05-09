#pragma once

// IScene — abstract scene interface + SceneContext shared data bag.
//
// SceneContext is passed by reference every frame so scenes can access
// the graphics device, renderer, current frame index, and resolved
// viewport dimensions (already clamped to window size by App).

#include "ECS/ECS.h"
#include "Graphics/RenderTypes.h"   // FrameIndex, RHI::CommandList

#include <functional>
#include <memory>

class IGraphicsDevice;
class Renderer;
class IScene;
namespace Resource
{
    class ResourceManager;
    class TextureSystem;
    class MeshSystem;
    class MeshLibrary;
    class MaterialSystem;
    class AssetManager;
}

// ---------------------------------------------------------------------------
// SceneContext — shared services/data accessible to every scene.
// Owned and updated by App::Run() each frame.
// ---------------------------------------------------------------------------
struct SceneContext
{
    IGraphicsDevice& gfx;
    Renderer&        renderer;
    FrameIndex       frame     = 0;
    float            deltaTime = 0.0f; // elapsed seconds since previous frame
    unsigned int     viewportW = 0;   // resolved viewport width  (>0 always)
    unsigned int     viewportH = 0;   // resolved viewport height (>0 always)

    // Viewport camera-drag input (right-mouse held over the Viewport panel).
    // Populated by App from the previous frame's EditorLayer state.
    float mouseViewportDX        = 0.f;
    float mouseViewportDY        = 0.f;
    bool  viewportRightMouseHeld = false;

    // Active World for this frame. Set by the active scene's Update().
    // Read by App to pass to EditorLayer::SetWorld() before OnUIRender().
    World* world = nullptr;

    // Resource systems — owned by App, valid for the entire session.
    Resource::ResourceManager* resourceMgr = nullptr;
    Resource::TextureSystem*   textureSys  = nullptr;
    Resource::MeshSystem*      meshSys     = nullptr;
    Resource::MeshLibrary*     meshLib     = nullptr;  // new P1-P6 mesh-pool store
    Resource::MaterialSystem*  matSys      = nullptr;

    // Unified asset cache (wraps meshSys + textureSys with path-based dedup).
    Resource::AssetManager*    assetMgr    = nullptr;

    // Scene transition request — call from inside an IScene::Update to
    // replace the current scene with a new one. App processes the request
    // AFTER the current Update returns (so SceneManager's stack isn't
    // mutated mid-iteration). null until App wires it up.
    std::function<void(std::unique_ptr<IScene>)> requestReplaceScene = nullptr;
};

// ---------------------------------------------------------------------------
// IScene — gameplay-specific lifecycle hook.
//
// Post-refactor: IScene NO LONGER owns the World, ECS systems, or rendering.
// App owns those at engine level. Each IScene is just a way to attach
// per-level gameplay logic (intro cutscene, boss room AI, hub area script
// orchestration). Use ctx->world to operate on the App-owned ECS.
//
// Typical Init responsibilities:
//   - spawn level-specific entities (camera, lights, props)
//   - register scene-specific Lua scripts
//   - subscribe to events
// Typical Update responsibilities (rare — most logic lives in scripts/systems):
//   - drive scripted level events (countdowns, scripted camera moves)
// Typical Shutdown responsibilities:
//   - clear entities this scene spawned
//   - unsubscribe events
// ---------------------------------------------------------------------------
class IScene
{
public:
    virtual ~IScene() = default;

    virtual const char* GetName() const = 0;

    // Called once when the scene is pushed onto the SceneManager stack.
    virtual void Init    (SceneContext* ctx) = 0;

    // Called every frame for gameplay-specific scene logic. Engine systems
    // (script/physics/transform/animation) tick in App regardless of which
    // scene is active.
    virtual void Update  (float dt) = 0;

    // Called when the scene is popped off the stack; clean up scene-spawned
    // entities and listeners. The shared World keeps living afterwards.
    virtual void Shutdown() = 0;

    // Optional ImGui hook — called between EditorLayer::BeginImGuiFrame and
    // EndImGuiFrame so the active scene can render its own panels. Default
    // is a no-op so existing scenes don't have to opt in. Used by
    // ShaderLabScene to surface mesh / lights / HDRI / turntable controls
    // without polluting shared EditorLayer code (per design doc §6.3 —
    // tool-specific UI lives in tool-specific source).
    virtual void OnUIRender(SceneContext& /*ctx*/) {}
protected:
	SceneContext* m_ctx = nullptr; // cached from Init() for Shutdown()
};
