#pragma once

// ScriptSystem — owns the Lua VM and three script categories defined in
// DesignMd/Script_Architecture_Reference.md:
//
//   * Logic   — entity-bound; one template per .lua file, one instance per
//               entity. Template returns a table; engine calls
//               `inst:OnSpawn(entity)`, `inst:OnUpdate(dt)`, `inst:OnDestroy()`.
//   * System  — global singleton; loaded once. Returns a table with
//               `OnInit/OnUpdate/OnShutdown`.
//   * Service — global singleton; stateless. Returns a table of pure functions.
//
// Plus Config (Lua-as-data): caller does Engine.LoadConfig("path") from Lua
// or LoadConfig(path) from C++ — no engine-side storage.
//
// All actual sol::table state lives in the Lua state under registry-style
// tables (`__logic_templates`, `__logic_instances`, `__systems`, `__services`)
// so this header is sol-free.

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"    // LocalTransform (LogicState::baseTransform)
#include "Scripting/ScriptExposedVar.h" // ScriptVarValue / ScriptVarDesc
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

    // Borrowed access to the live sol::state. Used by the BT runtime / UI
    // bindings to share a single Lua VM.
    // Null until Initialize() runs; callers must guard.
    sol::state* GetLua() const { return m_lua; }

    // ---- Game time scale ----------------------------------------------------
    void  SetTimeScale(float s)             { m_timeScale = s < 0.f ? 0.f : s; }
    float GetTimeScale() const              { return m_timeScale; }

    // Tick scheduled `Engine.AfterDelay` callbacks. Pass REAL (unscaled) dt so
    // hit-stop timers expire even while m_timeScale is near zero.
    void TickTimers(float realDt);

    // ---- Boot scan ----------------------------------------------------------
    // Walk root/services/*.lua then root/systems/*.lua and register each.
    // Logic templates are NOT scanned — they load lazily on first
    // ScriptComponent sight (entity scripts can live anywhere).
    void ScanScriptDirectory(const std::string& root);

    // ---- System scripts (singleton, ordered, OnInit/OnUpdate/OnShutdown) ----
    // Idempotent: re-loading the same path swaps the system in place.
    bool LoadSystem(const std::string& path);
    bool RemoveSystem(const std::string& name);

    // ---- Service scripts (singleton, stateless, no callbacks) --------------
    bool LoadService(const std::string& path);

    // ---- UI scripts (singleton, mirrors Service; lives in scripts/ui/) -----
    // Auto-loaded by ScanScriptDirectory. Caller uses Engine.GetUIScript(name)
    // and drives the returned table's :Open()/:Close() conventions.
    bool LoadUIScript(const std::string& path);

    // ---- Animation event dispatch (called by AnimationSystem / gameplay) ---
    // Fires OnAnimEvent(self, name, payload) on the entity's Logic instance.
    // Lua side equivalent: Engine.PublishAnimEvent(entityId, name, payload).
    void DispatchAnimEvent(Entity e, const std::string& name);

    // ---- Editor-exposed script variables (Unity/Unreal-style) --------------
    // Returns the parsed SCHEMA (type + default + UI metadata) for a Logic
    // script's `exposed` table. Lazily loads the template on first request so
    // the inspector can show variables before the script ever ticks. Returns an
    // empty vector for scripts with no `exposed` table (or on load failure).
    const std::vector<ScriptVarDesc>& GetExposedSchema(const std::string& path);

    // Push per-entity exposed-var values onto the entity's LIVE Lua instance
    // (used by the editor to reflect inspector edits during Play). No-op if the
    // entity has no live instance yet (edit mode) — values still persist on the
    // ScriptComponent and get injected at spawn. `overrides` is the component's
    // var map; absent keys fall back to schema defaults.
    void ApplyExposedVars(Entity e,
                          const std::unordered_map<std::string, ScriptVarValue>& overrides);

private:
    // Per-entity Logic state. The actual instance sol::table lives in
    // (*m_lua)["__logic_instances"][entity]; this struct is just metadata.
    struct LogicState
    {
        bool        loaded     = false;     // template loaded for this path
        bool        initCalled = false;     // OnSpawn fired
        std::string path;                   // which template this instance came from
        std::string lastError;

        // Baseline snapshot of the entity's LocalTransform captured the first
        // time its script runs. Lua scripts that oscillate around a rest pose
        // (sin-wave bob, orbit, breathe…) should read GetBasePosition() and
        // write absolute values, NOT integrate. Preserved across hot reloads.
        LocalTransform baseTransform;
        bool           baseCaptured = false;
    };

    // Per-path template metadata. Prototype table lives in
    // (*m_lua)["__logic_templates"][path].
    struct LogicTemplate
    {
        std::string path;
        bool        hasSpawn   = false;
        bool        hasUpdate  = false;
        bool        hasDestroy = false;
        std::string lastError;

        // Parsed `exposed` table — editor-visible variable schema. Empty when
        // the script declares no exposed variables. Order is stable (sorted by
        // name) so the inspector layout doesn't jump between loads.
        std::vector<ScriptVarDesc> exposed;
    };

    // System / Service entries — actual table lives in lua under
    // __systems[name] / __services[name].
    struct SystemEntry
    {
        std::string name;       // file stem
        std::string path;
        bool        loaded      = false;
        bool        initCalled  = false;
        bool        hasUpdate   = false;
        bool        hasShutdown = false;
        std::string lastError;
    };
    struct ServiceEntry
    {
        std::string name;       // file stem
        std::string path;
        bool        loaded      = false;
        std::string lastError;
    };

    void RegisterBindings();
    void RegisterCppEventBridges();        // C++ EventBus → Lua bus

    // Logic template / instance plumbing.
    bool LoadLogicTemplate(const std::string& path);
    // creates instance via metatable, injects exposed vars, fires OnSpawn.
    // `overrides` (may be null) supplies per-entity exposed-var values.
    bool EnsureLogicInstance(Entity e, const std::string& path,
                             const std::unordered_map<std::string, ScriptVarValue>* overrides);
    void DestroyLogicInstance(Entity e);                          // fires OnDestroy, drops instance

    // Parse a template table's `exposed` field into a ScriptVarDesc vector.
    // sol-typed body lives in the .cpp. Called from LoadLogicTemplate.
    void ParseExposedSchema(const std::string& path, std::vector<ScriptVarDesc>& out);
    // Write the resolved exposed-var values (default or override) onto a live
    // instance table. Shared by EnsureLogicInstance and ApplyExposedVars.
    void InjectExposedVars(Entity e, const std::string& path,
                           const std::unordered_map<std::string, ScriptVarValue>* overrides);

    void SweepDestroyed(World& world);     // run OnDestroy for dead/detached entities

public:
    // Wires a listener onto `world` so OnDestroy fires synchronously with
    // World::DestroyEntity instead of on next Update's poll. Without this, a
    // CreateEntity that recycles the freed ID in the same frame as the destroy
    // would leave the old entity's Lua state bound to the new entity until
    // SweepDestroyed catches up.
    void BindWorld(World& world);
    void UnbindWorld();

private:
    void OnEntityDestroyed(World& world, Entity e);  // listener body
    void DispatchLuaEvents();              // drain LuaBus::pending once

    // System / Service shutdown helpers (call OnShutdown if present, drop table).
    void TeardownSystem(SystemEntry& sys);

    struct LuaBus;                          // pImpl — holds sol::* storage

    sol::state*                                      m_lua = nullptr;

    // Logic — per-entity instance metadata; templates keyed by file path.
    std::unordered_map<Entity, LogicState>           m_states;
    std::unordered_map<std::string, LogicTemplate>   m_logicTemplates;

    // Systems are vector for stable insertion order (load order = tick order
    // by default; doc §6.2 manifest-driven ordering can be added later).
    std::vector<SystemEntry>                         m_systems;
    std::unordered_map<std::string, ServiceEntry>    m_services;

    // UI script registry — same shape as services; separate map so the
    // category is preserved (Editor / tooling can list them apart).
    std::unordered_map<std::string, ServiceEntry>    m_uiScripts;

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
