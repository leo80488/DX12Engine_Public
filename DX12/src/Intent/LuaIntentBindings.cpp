#include "Intent/LuaIntentBindings.h"

#include "ECS/ECS.h"
#include "ECS/CharacterControllerComponent.h"
#include "Nav/NavComponents.h"
#include "System/Log.h"

#include <sol/sol.hpp>

namespace Intent
{

namespace {

// Helper: fetch (or create) the NavAgentComponent on `e`. Auto-promote
// pattern from feedback_ecs_pool_lazy_create — EnsurePool first so
// AddComponent on a poolless type doesn't take the slow path.
inline NavAgentComponent& GetOrCreateNavAgent(::World& world, Entity e)
{
    if (auto* p = world.GetComponent<NavAgentComponent>(e)) return *p;
    world.EnsurePool<NavAgentComponent>();
    NavAgentComponent fresh{};
    world.AddComponent<NavAgentComponent>(e, fresh);
    return *world.GetComponent<NavAgentComponent>(e);
}

} // namespace

void RegisterLuaIntentBindings(sol::state& lua, ::World& world)
{
    sol::table tbl = lua.create_table();

    // Intent.MoveTo(eid, x, y, z) — publish a tactical move destination.
    // Stored on NavAgentComponent.destination (the tactical layer per
    // DesignMd §1.1). NavAgentSystem turns this into a path query and
    // writes CharacterController.desiredHorizontalVelocity (or, when no
    // CCC is present, writes LocalTransform.translation directly via
    // the legacy fallback path — see NavAgentSystem.cpp).
    //
    // NOTE: do NOT set pathDirty here. The repath gate in NavAgentSystem
    // checks `Dist2XZ(destination, lastPlannedTarget) > repathDistance²`,
    // so a same-destination MoveTo call (which BT actions issue every
    // tick) is naturally cheap. Setting pathDirty unconditionally caused
    // FindPath to fire every BT tick and reset nextWaypoint=1, which
    // produced visible jitter (the agent kept re-targeting the first
    // corner).
    tbl.set_function("MoveTo",
        [&world](std::uint32_t entityId, float x, float y, float z) -> bool
        {
            Entity e = static_cast<Entity>(entityId);
            auto& a = GetOrCreateNavAgent(world, e);
            a.destination    = { x, y, z };
            a.hasDestination = true;
            return true;
        });

    // Intent.Stop(eid) — drop the active move destination. NavAgentSystem
    // zeros desiredHorizontalVelocity on its next tick. Doesn't promote a
    // NavAgentComponent — without one there's nothing to stop.
    tbl.set_function("Stop",
        [&world](std::uint32_t entityId) -> bool
        {
            Entity e = static_cast<Entity>(entityId);
            auto* a = world.GetComponent<NavAgentComponent>(e);
            if (!a) return false;
            a->hasDestination = false;
            a->pathValid      = false;
            return true;
        });

    // Intent.LookAt(eid, x, y, z) — face this world point. Sets the
    // NavAgent's facing mode to FaceTarget AND writes lookTarget;
    // NavAgentSystem honours the override even when the agent is
    // stationary (combat strafe pattern).
    tbl.set_function("LookAt",
        [&world](std::uint32_t entityId, float x, float y, float z) -> bool
        {
            Entity e = static_cast<Entity>(entityId);
            auto& a = GetOrCreateNavAgent(world, e);
            a.lookTarget = { x, y, z };
            a.useLookAt  = true;
            a.facingMode = NavFacingMode::FaceTarget;
            return true;
        });

    // Intent.ClearLookAt(eid) — release facing override; rotation reverts
    // to FaceMovement (the default — align with steering direction).
    tbl.set_function("ClearLookAt",
        [&world](std::uint32_t entityId) -> bool
        {
            Entity e = static_cast<Entity>(entityId);
            auto* a = world.GetComponent<NavAgentComponent>(e);
            if (!a) return false;
            a->useLookAt  = false;
            a->facingMode = NavFacingMode::FaceMovement;
            return true;
        });

    // Intent.HasMoveTarget(eid) — convenience query for BTs that branch on
    // "am I currently under move orders".
    tbl.set_function("HasMoveTarget",
        [&world](std::uint32_t entityId) -> bool
        {
            Entity e = static_cast<Entity>(entityId);
            const auto* a = world.GetComponent<NavAgentComponent>(e);
            return a && a->hasDestination;
        });

    // Intent.RequestStop(eid) — escape hatch for cutscene / teleport: zero
    // CCC velocity on the next physics step. Pairs naturally with Intent.Stop
    // (which clears nav) so callers can fully freeze the entity:
    //   Intent.Stop(e); Intent.RequestStop(e)
    tbl.set_function("RequestStop",
        [&world](std::uint32_t entityId) -> bool
        {
            Entity e = static_cast<Entity>(entityId);
            auto* cc = world.GetComponent<CharacterControllerComponent>(e);
            if (!cc) return false;
            cc->instantStopRequested = true;
            return true;
        });

    lua["Intent"] = tbl;
    LOG_INFO("Intent: registered Lua bindings");
}

} // namespace Intent
