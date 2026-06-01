#pragma once

// AITacticalSystem — translates AIIntentComponent (strategic) into
// NavAgentComponent.destination (tactical) and CharacterController
// facing hints. Runs between BehaviorTreeSystem and NavAgentSystem in
// the AI phase (DesignMd/character_movement_architecture.md §3.1).
//
//   BT writes AIIntent.currentGoal (Attack, Patrol, ...)
//         ↓
//   AITacticalSystem reads AIIntent → writes NavAgent.destination,
//                                              NavAgent.facingMode,
//                                              NavAgent.facingTarget
//         ↓
//   NavAgentSystem turns destination into CCC.desiredHorizontalVelocity
//         ↓
//   KCC step sweeps and slides
//
// Coexistence with direct Intent.MoveTo calls:
// BTs can still publish a tactical destination directly via Lua
// `Intent.MoveTo` (which writes NavAgent.destination). AITacticalSystem
// only acts on entities whose AIIntent.currentGoal is non-Idle. Pure
// patrol BTs that don't set a goal stay on the direct-write path.

class World;

namespace AI
{
    void AITacticalTick(::World& world, float dt);
}
