#pragma once

// LuaIntentBindings — exposes `Intent.*` on the engine's sol::state. The
// tactical-intent producer interface; BT actions / future click-to-move /
// cutscene write here; NavAgentSystem reads.
//
// Lua surface (auto-creates NavAgentComponent on first call):
//   Intent.MoveTo(entityId, x, y, z)    -> bool
//   Intent.Stop(entityId)               -> bool
//   Intent.LookAt(entityId, x, y, z)    -> bool   (sets facingMode=FaceTarget)
//   Intent.ClearLookAt(entityId)        -> bool   (reverts to FaceMovement)
//   Intent.HasMoveTarget(entityId)      -> bool
//   Intent.RequestStop(entityId)        -> bool   (cutscene escape hatch —
//                                                  zeroes CCC velocity next step)
//
// All writers auto-promote a NavAgentComponent onto the entity on first
// touch — scripts don't need to AddComponent explicitly.
//
// For strategic intent ("attack X", "patrol Y") use Intent/LuaAIBindings.cpp
// (`AI.SetGoal` etc.) — that writes AIIntentComponent and AITacticalSystem
// translates the goal into the equivalent NavAgent.destination + facingMode.

namespace sol { class state; }
class World;   // global ::World

namespace Intent
{
    void RegisterLuaIntentBindings(sol::state& lua, ::World& world);
}
