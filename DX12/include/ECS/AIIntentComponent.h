#pragma once

// AIIntentComponent — the *strategic layer* of the layered movement model
// (DesignMd/character_movement_architecture.md §1.1, §2.3):
//
//   AIIntentComponent  ← this file. "What do I want to achieve right now?"
//        ↓ AITacticalSystem translates goal → NavAgent.destination + CCC.desiredFacing
//   NavAgentComponent  (tactical: where to go, how to face)
//        ↓ NavAgentSystem
//   CharacterControllerComponent  (execution: Jolt sweep)
//
// The strategic intent's lifetime is **much longer than path lifetime**. A
// path corner advances every ~1s; a "go attack the player" goal might last
// 30s. They live in different components precisely so path replan never
// accidentally clears the strategic goal (DesignMd §7 坑 9).
//
// Producers (writers): BehaviorTreeSystem (via Lua BT leaves), gameplay
// scripted events, debug commands (`ai.force_goal`).
//
// Consumers (readers):
//   * AITacticalSystem  — turns Goal into NavAgent.destination
//   * AnimationSystem   — pick stance / combat anim from current_goal
//   * PerceptionSystem  — tighten perception cadence when in Attack/Flee
//   * AudioSystem       — emit battle-cry SFX on Attack entry
//
// The fact that multiple systems read this is the reason it's a separate
// component — burying the goal inside NavAgent or BehaviorTree would couple
// every reader to those subsystems.

#include "ECS/ECS.h"

#include <DirectXMath.h>

#include <cstdint>

enum class AIGoal : std::uint8_t
{
    Idle,         // no active intent
    Patrol,       // walk a route / area; goalPosition = next waypoint
    Investigate,  // move to a sound / sight cue; goalPosition = cue origin
    Attack,       // engage targetEntity; AITactical computes attack position
    Flee,         // run away from targetEntity (or last threat position)
    TakeCover,    // find/move to cover relative to targetEntity
    Follow,       // tail behind targetEntity at a comfortable distance
    UseObject,    // interact with targetEntity (lever, door, ...)
};

struct AIIntentComponent : ComponentBase
{
    // Currently-active strategic goal. BTSystem flips this on transitions;
    // most ticks just re-affirm the existing goal so AITactical can no-op.
    AIGoal             currentGoal     = AIGoal::Idle;

    // The entity this goal is aimed at (attack/flee/follow target, the
    // object to use, etc.). NullEntity when irrelevant (Idle, Patrol).
    Entity             targetEntity    = NullEntity;

    // World-space anchor for goals that operate on a position rather than
    // an entity (Patrol → next patrol point, Investigate → noise origin,
    // TakeCover → resolved cover slot, etc.). Ignored when targetEntity is
    // set and the goal uses it.
    DirectX::XMFLOAT3  goalPosition    = { 0.f, 0.f, 0.f };

    // Utility-AI priority — when multiple goals compete, BT picks highest.
    // For pure-FSM BTs this is informational only.
    float              goalPriority    = 0.f;

    // Time (ctx:Elapsed()) when the current goal was set. Used by AI
    // debugging overlays and "goal staleness" heuristics (e.g. Flee
    // expires after N seconds of safety).
    float              goalStartTime   = 0.f;
};
