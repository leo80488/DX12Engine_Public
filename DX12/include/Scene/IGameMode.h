#pragma once

// IGameMode — abstract gameplay-mode interface + GameModeContext shared data bag.
//
// Naming model (post-rename, 2026-05-25):
//   - The runtime ECS container is `World` (engine-lifetime, owned by App).
//   - A "scene" on disk is a `.iscene` asset deserialised by SceneSerializer.
//   - An `IGameMode` is the per-level *gameplay* logic hook stacked on a
//     `GameModeStack`. It does NOT own the World, ECS systems, or rendering;
//     those are engine-level concerns living on App.
//
// GameModeContext is passed by reference every frame so modes can access the
// graphics device, renderer, current frame index, and resolved viewport
// dimensions (already clamped to window size by App).

#include "ECS/ECS.h"
#include "Graphics/RenderTypes.h"   // FrameIndex, RHI::CommandList

#include <functional>
#include <memory>

class IGraphicsDevice;
class Renderer;
class IGameMode;
namespace Resource
{
    class ResourceManager;
    class TextureSystem;
    class MeshSystem;
    class MeshLibrary;
    class MaterialSystem;
    class AssetManager;
}
namespace Nav { class NavMeshSystem; }

// ---------------------------------------------------------------------------
// GameModeContext — shared services/data accessible to every game mode.
// Owned and updated by App::Run() each frame.
// ---------------------------------------------------------------------------
struct GameModeContext
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

    // Active World for this frame. Set by the active mode's Update().
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

    // Navmesh — owned by App. The Game build's scene loader (GameScene) feeds
    // the .iscene's baked navmesh path here so runtime FindPath / NavAgent
    // path-following are live. The editor does the equivalent in
    // EditorLayer::LoadWorldFromFile; without it the Game build leaves
    // Nav::IsReady() false and all AI path-following silently dies (enemies
    // load with full components but never move).
    Nav::NavMeshSystem*        navSys      = nullptr;

    // Mode transition request — call from inside an IGameMode::Update to
    // replace the current mode with a new one. App processes the request
    // AFTER the current Update returns (so GameModeStack isn't mutated
    // mid-iteration). null until App wires it up.
    std::function<void(std::unique_ptr<IGameMode>)> requestReplaceMode = nullptr;

    // Like requestReplaceMode, but routes the switch through the
    // SceneTransitionManager so it is wrapped in a fade-out -> loading screen ->
    // fade-in instead of an instant cut. Prefer this for player-facing scene
    // changes; requestReplaceMode stays the raw instant swap (and is what the
    // transition manager itself uses internally). null until App wires it up.
    std::function<void(std::unique_ptr<IGameMode>)> beginTransition = nullptr;
};

// ---------------------------------------------------------------------------
// IGameMode — gameplay-specific lifecycle hook.
//
// IGameMode does NOT own the World, ECS systems, or rendering. App owns those
// at engine level. Each IGameMode is just a way to attach per-level gameplay
// logic (intro cutscene, boss room AI, hub area script orchestration). Use
// ctx->world to operate on the App-owned ECS.
//
// Typical Init responsibilities:
//   - spawn level-specific entities (camera, lights, props)
//   - register mode-specific Lua scripts
//   - subscribe to events
// Typical Update responsibilities (rare — most logic lives in scripts/systems):
//   - drive scripted level events (countdowns, scripted camera moves)
// Typical Shutdown responsibilities:
//   - clear entities this mode spawned
//   - unsubscribe events
// ---------------------------------------------------------------------------
class IGameMode
{
public:
    virtual ~IGameMode() = default;

    virtual const char* GetName() const = 0;

    // Called once when the mode is pushed onto the GameModeStack.
    virtual void Init    (GameModeContext* ctx) = 0;

    // Called every frame for gameplay-specific mode logic. Engine systems
    // (script/physics/transform/animation) tick in App regardless of which
    // mode is active.
    virtual void Update  (float dt) = 0;

    // Called when the mode is popped off the stack; clean up mode-spawned
    // entities and listeners. The shared World keeps living afterwards.
    virtual void Shutdown() = 0;

    // Optional ImGui hook — called between EditorLayer::BeginImGuiFrame and
    // EndImGuiFrame so the active mode can render its own panels. Default is
    // a no-op so existing modes don't have to opt in. Used by ShaderLabScene
    // to surface mesh / lights / HDRI / turntable controls without polluting
    // shared EditorLayer code (per design doc §6.3 — tool-specific UI lives
    // in tool-specific source).
    virtual void OnUIRender(GameModeContext& /*ctx*/) {}
protected:
	GameModeContext* m_ctx = nullptr; // cached from Init() for Shutdown()
};
