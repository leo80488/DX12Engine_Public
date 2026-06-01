#include "Intent/LuaAIBindings.h"

#include "ECS/ECS.h"
#include "ECS/AIIntentComponent.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include <cstdint>
#include <string>

namespace Intent
{

namespace {

// Auto-promote helper — same pattern as the tactical Intent bindings.
inline AIIntentComponent& GetOrCreateAIIntent(::World& world, Entity e)
{
    if (auto* p = world.GetComponent<AIIntentComponent>(e)) return *p;
    world.EnsurePool<AIIntentComponent>();
    AIIntentComponent fresh{};
    world.AddComponent<AIIntentComponent>(e, fresh);
    return *world.GetComponent<AIIntentComponent>(e);
}

// Goal name (string from Lua) → enum. Returns Idle on unknown so a
// typo in BT script doesn't silently latch the previous goal.
AIGoal GoalFromName(const std::string& s)
{
    if (s == "Idle")        return AIGoal::Idle;
    if (s == "Patrol")      return AIGoal::Patrol;
    if (s == "Investigate") return AIGoal::Investigate;
    if (s == "Attack")      return AIGoal::Attack;
    if (s == "Flee")        return AIGoal::Flee;
    if (s == "TakeCover")   return AIGoal::TakeCover;
    if (s == "Follow")      return AIGoal::Follow;
    if (s == "UseObject")   return AIGoal::UseObject;
    return AIGoal::Idle;
}

const char* NameFromGoal(AIGoal g)
{
    switch (g)
    {
        case AIGoal::Idle:        return "Idle";
        case AIGoal::Patrol:      return "Patrol";
        case AIGoal::Investigate: return "Investigate";
        case AIGoal::Attack:      return "Attack";
        case AIGoal::Flee:        return "Flee";
        case AIGoal::TakeCover:   return "TakeCover";
        case AIGoal::Follow:      return "Follow";
        case AIGoal::UseObject:   return "UseObject";
    }
    return "Idle";
}

// Coerce sol::object → AIGoal. Accepts both numeric (AI.Goal.Attack) and
// string ("Attack") form so authors can use whichever reads better in
// context. Default to Idle on type mismatch.
AIGoal ParseGoal(const sol::object& o)
{
    if (o.is<int>())                  return static_cast<AIGoal>(o.as<int>());
    if (o.is<std::uint32_t>())        return static_cast<AIGoal>(o.as<std::uint32_t>());
    if (o.is<std::string>())          return GoalFromName(o.as<std::string>());
    return AIGoal::Idle;
}

} // namespace

void RegisterLuaAIBindings(sol::state& lua, ::World& world)
{
    sol::table tbl = lua.create_table();

    // AI.Goal — enum mirror table so BT leaves can write
    //   AI.SetGoal(me, AI.Goal.Attack, target)
    // instead of magic numbers / strings.
    {
        sol::table goals = lua.create_table();
        goals["Idle"]        = static_cast<int>(AIGoal::Idle);
        goals["Patrol"]      = static_cast<int>(AIGoal::Patrol);
        goals["Investigate"] = static_cast<int>(AIGoal::Investigate);
        goals["Attack"]      = static_cast<int>(AIGoal::Attack);
        goals["Flee"]        = static_cast<int>(AIGoal::Flee);
        goals["TakeCover"]   = static_cast<int>(AIGoal::TakeCover);
        goals["Follow"]      = static_cast<int>(AIGoal::Follow);
        goals["UseObject"]   = static_cast<int>(AIGoal::UseObject);
        tbl["Goal"] = goals;
    }

    // AI.SetGoal(eid, goal[, targetEntity[, gx, gy, gz]])
    // Auto-creates AIIntentComponent on first call. AITacticalSystem
    // reads from there next tick and writes NavAgent.destination /
    // facingMode. BT leaves should set the goal, not write nav fields
    // directly (DesignMd §6.2).
    tbl.set_function("SetGoal",
        [&world](sol::variadic_args args) -> bool
        {
            if (args.size() < 2) return false;
            const Entity e = static_cast<Entity>(args.get<std::uint32_t>(0));
            AIIntentComponent& intent = GetOrCreateAIIntent(world, e);
            intent.currentGoal = ParseGoal(args.get<sol::object>(1));

            if (args.size() >= 3)
                intent.targetEntity = static_cast<Entity>(args.get<std::uint32_t>(2));
            else
                intent.targetEntity = NullEntity;

            if (args.size() >= 6)
            {
                intent.goalPosition = {
                    args.get<float>(3),
                    args.get<float>(4),
                    args.get<float>(5)
                };
            }
            // goalStartTime should be the current sim time, but BT script
            // can update it via ctx:Elapsed() if it cares — we don't have
            // a clock handle here. Leave at default 0; debug overlays
            // tolerate it.
            return true;
        });

    // AI.ClearGoal(eid) — flip back to Idle. AITacticalSystem will clear
    // the agent's destination next tick so the entity halts.
    tbl.set_function("ClearGoal",
        [&world](std::uint32_t entityId) -> bool
        {
            Entity e = static_cast<Entity>(entityId);
            auto* p = world.GetComponent<AIIntentComponent>(e);
            if (!p) return false;
            p->currentGoal  = AIGoal::Idle;
            p->targetEntity = NullEntity;
            return true;
        });

    // AI.GetGoal(eid) -> string. Returns "Idle" when no AIIntent attached
    // (consistent with default state).
    tbl.set_function("GetGoal",
        [&world](std::uint32_t entityId) -> std::string
        {
            Entity e = static_cast<Entity>(entityId);
            const auto* p = world.GetComponent<AIIntentComponent>(e);
            return NameFromGoal(p ? p->currentGoal : AIGoal::Idle);
        });

    // AI.GetTarget(eid) -> entityId. Returns 0 (NullEntity) when unset.
    tbl.set_function("GetTarget",
        [&world](std::uint32_t entityId) -> std::uint32_t
        {
            Entity e = static_cast<Entity>(entityId);
            const auto* p = world.GetComponent<AIIntentComponent>(e);
            return static_cast<std::uint32_t>(p ? p->targetEntity : NullEntity);
        });

    lua["AI"] = tbl;
    LOG_INFO("AI: registered Lua bindings");
}

} // namespace Intent
