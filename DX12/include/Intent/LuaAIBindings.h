#pragma once

// LuaAIBindings — exposes `AI.*` on the engine's sol::state. The
// strategic-intent counterpart to `Intent.*` (tactical).
//
// Lua surface:
//   AI.SetGoal(entityId, goalNameOrId, targetEntity?, gx?, gy?, gz?)
//   AI.ClearGoal(entityId)
//   AI.GetGoal(entityId)                 -> "Idle" / "Attack" / ...
//   AI.GetTarget(entityId)               -> entityId (or 0 when unset)
//   AI.Goal                              -> table { Idle=0, Patrol=1, ... }
//
// Goal constants are exposed as `AI.Goal.Attack` etc. so BT leaves can
// write `AI.SetGoal(me, AI.Goal.Attack, player)` instead of magic strings.
// String names are also accepted by SetGoal for ergonomics.

namespace sol { class state; }
class World;

namespace Intent
{
    void RegisterLuaAIBindings(sol::state& lua, ::World& world);
}
