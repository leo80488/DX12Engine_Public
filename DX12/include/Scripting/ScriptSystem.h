#pragma once

// ScriptSystem — manages the Lua VM, per-entity script environments,
// and the Init/Update lifecycle. Call Update() once per frame.
//
// Usage:
//   ScriptSystem scriptSys;
//   scriptSys.Initialize();
//   // ... each frame:
//   scriptSys.Update(world, dt);

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"    // LocalTransform (ScriptState::baseTransform)
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <memory>

// Forward declare sol types to avoid pulling the heavy header here.
namespace sol { class state; }

class ScriptSystem
{
public:
    ScriptSystem();
    ~ScriptSystem();

    void Initialize();
    void Update(World& world, float dt);
    void CheckHotReload(World& world);
    void ClearAll();

    // Borrowed access to the live sol::state. Used by the BT runtime to
    // share a single Lua VM (hot-reload, AfterDelay, EventBus bridges,
    // TimeScale all stay coherent) instead of opening a parallel state.
    // Null until Initialize() runs; callers must guard.
    sol::state* GetLua() const { return m_lua; }

    // ---- Game time scale ----------------------------------------------------
    // 1.0 = normal, 0.0 = full freeze (hit stop), 0.5 = half-speed bullet-time.
    // Caller multiplies its dt by GetTimeScale() before stepping gameplay
    // systems (scripts, physics, animation). Editor pause supersedes this —
    // when the editor is paused the entire scene Update is skipped.
    void  SetTimeScale(float s)             { m_timeScale = s < 0.f ? 0.f : s; }
    float GetTimeScale() const              { return m_timeScale; }

    // Tick scheduled `Engine.AfterDelay` callbacks. Pass REAL (unscaled) dt so
    // hit-stop timers expire even while m_timeScale is near zero. Called once
    // per frame by the scene immediately before Update().
    void TickTimers(float realDt);

    // Register a script that lives outside the ECS — classic game-logic
    // owners (InputHandler, AbilitySystem, CombatSystem). Init() runs once
    // on first Update after registration; Update(dt) runs every frame.
    // Re-registering the same path is a no-op.
    void AddGlobalScript(const std::string& path);
    void RemoveGlobalScript(const std::string& path);

private:
    struct ScriptState
    {
        bool loaded      = false;
        bool hasInit     = false;
        bool hasUpdate   = false;
        bool hasDestroy  = false;   // Lua OnDestroy present
        bool initCalled  = false;
        std::string path;
        std::string lastError;

        // Baseline snapshot of the entity's LocalTransform captured the first
        // time its script runs. Lua scripts that oscillate around a rest pose
        // (sin-wave bob, orbit, breathe…) should read GetBasePosition() and
        // write absolute values, NOT integrate `pos = pos + delta * dt` — the
        // latter accumulates FP error and drifts whenever the script pauses
        // and resumes. Preserved across hot reloads so reloading a script
        // does not snap the baseline to the currently-drifted position.
        LocalTransform baseTransform;
        bool           baseCaptured = false;
    };

    void RegisterBindings();
    void RegisterCppEventBridges();        // C++ EventBus → Lua bus
    void LoadScript(Entity e, const std::string& path, World& world);
    void LoadGlobalScript(const std::string& path);
    void SweepDestroyed(World& world);     // run OnDestroy for dead/detached entities

public:
    // Reactive per-entity cleanup. Wires a listener onto `world` so the
    // Lua OnDestroy hook fires on the frame the entity is destroyed (instead
    // of next Update's SweepDestroyed poll). Without this, a CreateEntity
    // that recycles the freed ID in the same frame as the destroy would
    // leave the old entity's Lua state bound to the new entity until
    // SweepDestroyed catches up — and if the new entity loads its own
    // ScriptComponent immediately, the sweep would skip erasure entirely.
    void BindWorld(World& world);
    void UnbindWorld();

private:
    void OnEntityDestroyed(World& world, Entity e);  // listener body
    void DispatchLuaEvents();              // drain LuaBus::pending once

    struct LuaBus;                          // pImpl — holds sol::* storage

    sol::state*                                      m_lua = nullptr;
    std::unordered_map<Entity, ScriptState>          m_states;
    // Global scripts keyed by their file path — path doubles as the identity
    // since a global script is a singleton by role, not by multiplicity.
    std::unordered_map<std::string, ScriptState>     m_globalStates;
    std::unordered_map<std::string,
        std::filesystem::file_time_type>             m_fileTimestamps;
    float m_elapsed = 0.f;

    // Set at the top of Update() so Engine.* Lua bindings registered once in
    // RegisterBindings can reach the current frame's World via a stable
    // `this` capture. Null outside an Update call — Lua functions guard on it.
    World* m_world = nullptr;

    struct TimerQueue;
    std::unique_ptr<TimerQueue> m_timers;   // pImpl — holds sol::* callbacks
    float                       m_timeScale = 1.f;

    std::unique_ptr<LuaBus> m_luaBus;       // string-keyed event queue
    // Each entry unsubscribes one bridge from C++ EventBus. Bridges capture
    // `this` and the Lua state, so we must detach them before ScriptSystem
    // (and m_lua) get torn down.
    std::vector<std::function<void()>> m_cppBridgeUnsubscribers;

    // BindWorld subscription bookkeeping — stored so UnbindWorld can remove
    // exactly the listener we added.
    World*   m_boundWorld                 = nullptr;
    uint32_t m_entityDestroyListenerId    = 0;
};
