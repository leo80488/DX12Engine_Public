#pragma once

// AISystem — drives BT execution for every AIComponent each frame.
//
// Tick order (see App::Run):
//   ScriptSystem.Update      // Lua scripts may set blackboard entries
//     ↓
//   AISystem.Update          // BT writes MoveDestination, AnimRequest, ...
//     ↓
//   PhysicsSystem.Update     // Movement / contact resolution
//     ↓
//   AnimationSystem.Update   // Pose evaluation
//     ↓
//   TransformSystem.Propagate
//     ↓
//   Renderer.Render
//
// AI ticks BEFORE physics so a BT-issued MoveDestination is consumed the
// same frame, and AFTER scripts so Lua can stage perception data first.

#include "ECS/ECS.h"
#include "AI/ActionRegistry.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace sol { class state; }

namespace AI
{
    class BTAsset;

    class AISystem
    {
    public:
        AISystem();
        ~AISystem();

        // One-time wiring. Borrows ScriptSystem's sol::state — passing
        // null disables Lua loading and dispatch entirely.
        void Init(sol::state* lua);

        // Per-frame tick. Calls Tick on every enabled AIComponent.
        void Update(World& world, float dt);

        // Synchronous load. Reads the file, runs it inside the shared
        // sol::state (which yields a Lua table), recursively converts
        // the table into a C++ node tree, assigns NodeIds, and caches
        // by path. Returns nullptr on parse failure. Subsequent calls
        // with the same path return the cached shared_ptr.
        std::shared_ptr<BTAsset> AcquireTree(const std::string& path);

        // Force-reparse a tree even if it's already cached. Updates the
        // path → asset cache so subsequent AcquireTree calls return the
        // fresh asset. Existing AIComponent::tree shared_ptrs are NOT
        // patched in place — callers must assign the returned pointer
        // themselves (or rely on CheckHotReload, which does the world-
        // wide patch via mtime polling). Returns nullptr on parse failure
        // and leaves the cached entry untouched in that case.
        std::shared_ptr<BTAsset> ReloadTree(const std::string& path);

        // Watch loaded BT files; if the source mtime changed, re-parse
        // and stamp every AIComponent that uses that path. Per-instance
        // state is reset so the new tree starts cleanly. Mirrors the
        // ScriptSystem::CheckHotReload contract — call once per frame.
        void CheckHotReload(World& world);

        ActionRegistry& Actions() { return m_actions; }

        // Total elapsed time since Init — exposed for cooldown decorators
        // that prefer absolute time over delta accumulation.
        float Elapsed() const { return m_elapsed; }

    private:
        void TickEntity(World& world, Entity e, float dt);
        void OnTreeReloaded(World& world, const std::string& path);

        // Internal: read+parse the file. Returns nullptr and logs on
        // failure. Always reads from disk — caller is responsible for
        // cache lookup.
        std::shared_ptr<BTAsset> ParseFromFile(const std::string& path);

        sol::state*     m_lua = nullptr;
        ActionRegistry  m_actions;

        // path → shared tree. Many AIComponents alias the same shared_ptr
        // so a Tier-1 enemy spawned 200× shares one BT.
        std::unordered_map<std::string, std::shared_ptr<BTAsset>> m_loaded;

        // Mtime watch for hot reload (mirrors ScriptSystem).
        std::unordered_map<std::string, std::filesystem::file_time_type> m_fileTimestamps;

        float m_elapsed = 0.f;
    };
}
