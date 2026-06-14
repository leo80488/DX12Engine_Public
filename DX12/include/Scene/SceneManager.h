#pragma once

// SceneManager — the data-driven scene registry + load orchestrator.
//
// This is the modern-engine replacement for the hardcoded TitleScene /
// GameScene / EndScene C++ classes. A "scene" is now pure DATA:
//   * content   — a .iscene file (entities/components, authored in the editor)
//   * behaviour — an optional Lua "scene script" (OnSceneEnter/Update/Exit)
//   * identity  — a NAME registered in the project manifest (game.json)
//
// Scenes are connected the way Unity/Godot do it: gameplay code (Lua) calls
// `Scene.Load("name")`; the manager resolves the name → .iscene path via the
// registry, wraps the swap in the existing fade transition, and on load tells
// the ScriptSystem which scene script is now active.
//
// SceneManager does NOT own the World — App does. It is a stateless-ish service
// (like NavMeshSystem) wired with references to the engine subsystems it needs.
//
// Threading: all entry points run on the main thread (called from the App loop,
// a DataScene::Init during the transition drain, or a Lua binding inside
// ScriptSystem::Update). ActivateScene performs a blocking load.

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>

class IGraphicsDevice;
class Renderer;
class ScriptSystem;
class IGameMode;
class World;

namespace Resource    { class AssetManager; class AnimationClipSystem; }
namespace Nav         { class NavMeshSystem; }
namespace DX12Physics { class PhysicsSystem; }

class SceneManager
{
public:
    // One registered scene: where its content + behaviour live.
    struct SceneEntry
    {
        std::string path;    // .iscene file (content)
        std::string script;  // Lua scene script (behaviour) — manifest fallback
                             // when the .iscene itself declares no sceneScript
    };

    using ReplaceModeFn = std::function<void(std::unique_ptr<IGameMode>)>;

    SceneManager() = default;

    // Wire the engine subsystems SceneManager needs to perform a load. Called
    // once by App during startup, after the systems exist.
    void Configure(IGraphicsDevice* gfx, Renderer* renderer, World* world,
                   Resource::AssetManager* assetMgr,
                   Resource::AnimationClipSystem* animClipSys,
                   Nav::NavMeshSystem* navSys, ScriptSystem* scriptSys,
                   DX12Physics::PhysicsSystem* physics);

    // Transition hooks (App owns the actual GameModeStack + fade manager).
    //   begin   — wraps the swap in fade-out → loading → fade-in (player-facing)
    //   replace — raw instant swap (no fade)
    // Both take a freshly-constructed IGameMode (a DataScene) to push.
    void SetTransitionHooks(ReplaceModeFn begin, ReplaceModeFn replace);

    // ---- Registry (project manifest) --------------------------------------
    // Parse game.json: "startup_scene" (name or path) + optional "scenes" map
    // { "name": "path.iscene" } or { "name": { "path":..., "script":... } }.
    // Back-compatible: a manifest with only "startup_scene" still works (the
    // startup is then a direct path with no registry).
    bool LoadManifest(const std::string& gameJsonPath);

    // Resolve a scene NAME (registry key) or a raw .iscene PATH to its concrete
    // path + manifest script fallback. Returns false if a name isn't registered
    // and isn't a path that exists. outName is the canonical scene name.
    bool ResolveScene(const std::string& nameOrPath,
                      std::string& outPath, std::string& outScript,
                      std::string& outName) const;

    // The startup scene name/path declared in the manifest (empty if none).
    const std::string& StartupScene() const { return m_startupScene; }

    // All registered scene names (for editor UI / Lua introspection).
    std::vector<std::string> SceneNames() const;

    // ---- Loading ----------------------------------------------------------
    // BLOCKING: clear the World and load `nameOrPath` into it, then mark its
    // scene script active. Mirrors the editor's safe world-reload order
    // (WaitIdle → Renderer::OnWorldClear → LoadScene → navmesh). Called by
    // DataScene::Init. Returns false on hard failure (spawns a default world
    // as a fallback so the viewport is never black).
    bool ActivateScene(const std::string& nameOrPath);

    // Request a (faded) scene change from gameplay code. Resolves the name,
    // then routes a new DataScene through the transition hooks. `fade=false`
    // does an instant cut. This is what Lua's Scene.Load() calls.
    void RequestLoad(const std::string& nameOrPath, bool fade = true);

    // Re-load the current scene from disk (Scene.Reload()).
    void Reload(bool fade = true);

    const std::string& CurrentScene() const { return m_currentName; }
    const std::string& CurrentScenePath() const { return m_currentPath; }

private:
    // Wired subsystems (App-owned; valid for the whole session).
    IGraphicsDevice*               m_gfx         = nullptr;
    Renderer*                      m_renderer    = nullptr;
    World*                         m_world       = nullptr;
    Resource::AssetManager*        m_assetMgr    = nullptr;
    Resource::AnimationClipSystem* m_animClipSys = nullptr;
    Nav::NavMeshSystem*            m_navSys      = nullptr;
    ScriptSystem*                  m_scriptSys   = nullptr;
    DX12Physics::PhysicsSystem*    m_physics     = nullptr;

    ReplaceModeFn m_beginTransition;   // faded swap
    ReplaceModeFn m_requestReplace;    // instant swap

    std::unordered_map<std::string, SceneEntry> m_registry;  // name → entry
    std::string m_startupScene;        // name or path

    std::string m_currentName;         // active scene name (or path stem if unnamed)
    std::string m_currentPath;         // active .iscene path
    // The exact id last passed to ActivateScene (a registry name OR a raw
    // path). Reload() reuses THIS so it round-trips identically — m_currentName
    // is a lossy stem for path-loaded scenes and would not re-resolve.
    std::string m_currentRequest;
};
