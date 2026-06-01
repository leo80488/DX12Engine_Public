#pragma once

// NavAgentComponent — the *tactical layer* in the layered movement model
// (DesignMd/character_movement_architecture.md §1.1):
//
//   AIIntentComponent  (strategic: what do I want to achieve?)
//        ↓ AITacticalSystem translates goal → destination
//   NavAgentComponent  (tactical: where am I going, how do I get there?)  ← this file
//        ↓ NavAgentSystem turns destination + path into desired velocity
//   CharacterControllerComponent  (execution: physics-aware motor)
//        ↓ KCC step sweep-and-slides via Jolt CharacterVirtual
//   LocalTransform
//
// What this component owns:
//   * destination — the world-space goal (single point per agent)
//   * path        — corner list from NavMesh.FindPath
//   * lookTarget  — optional facing override (combat strafe-while-facing)
//   * facingMode  — disambiguates who decides "where we face" this tick
//   * state       — Idle / Computing / Following / Arrived / Failed /
//                   TraversingOffMeshLink
//   * stuck detection scratch
//
// What this component does NOT own:
//   * Position / rotation (LocalTransform)
//   * Velocity, grounded state, collision (CharacterControllerComponent)
//   * High-level goal (AIIntentComponent)
//
// Lua surface: BT actions call `Intent.MoveTo / Intent.Stop / Intent.LookAt
// / Intent.ClearLookAt` (see src/Intent/LuaIntentBindings.cpp) which write
// the corresponding NavAgent fields. The Lua name "Intent" survives the
// IntentComponent → NavAgent fold because BT scripts speak in tactical-
// intent verbs ("move to point X, face Y") that map 1:1 onto this layer.

#include "ECS/ECS.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

// Where the agent should be facing this tick. NavAgentSystem reads
// `facingMode` and chooses the rotation target accordingly. See
// DesignMd §7 坑 7 (facing conflicts).
enum class NavFacingMode : std::uint8_t
{
    FaceMovement,   // align with the steering direction (default — walking forward)
    FaceTarget,     // face `lookTarget` regardless of motion (combat strafe)
    Manual,         // hands-off — some other system owns rotation this tick
};

// High-level path-follow state. Driven by NavAgentSystem; readable by BT
// conditions ("am I still pathing?") and debug overlays.
enum class NavAgentState : std::uint8_t
{
    Idle,                   // no destination
    Computing,              // FindPath issued this tick
    Following,              // walking the path
    Arrived,                // reached final corner
    Failed,                 // FindPath returned no valid path
    TraversingOffMeshLink,  // CCC.mode == LaunchedTraversal — path-following paused
};

struct NavAgentComponent : ComponentBase
{
    // ===== Tactical intent (writers: AITacticalSystem, BT via Intent.MoveTo) ====
    // World-space goal point. NavAgentSystem path-finds toward this each
    // tick (with a repath gate so we don't FindPath at 60Hz).
    DirectX::XMFLOAT3 destination     = { 0.0f, 0.0f, 0.0f };
    bool              hasDestination  = false;
    // Look-at override. When useLookAt is true AND facingMode != FaceMovement,
    // NavAgentSystem rotates toward this point. Strafe-around-player combat
    // uses this in tandem with facingMode = FaceTarget.
    DirectX::XMFLOAT3 lookTarget      = { 0.0f, 0.0f, 0.0f };
    bool              useLookAt       = false;
    // What decides facing this tick. Defaults to FaceMovement (the natural
    // "walk forward, look forward" behaviour). AITacticalSystem flips this
    // to FaceTarget for combat engagements; cutscenes use Manual.
    NavFacingMode     facingMode      = NavFacingMode::FaceMovement;
    // Entity to face when facingMode == FaceTarget and lookTarget isn't
    // explicitly provided. NavAgentSystem reads GlobalTransform from this
    // entity. NullEntity disables.
    Entity            facingTarget    = NullEntity;

    // ===== Steering tunables (Inspector-editable, scene-saved) ============
    float speed             = 3.0f;   // desired horizontal speed when path active (m/s)
    float arriveRadius      = 0.25f;  // advance to next waypoint within this XZ distance
    float slowdownRadius    = 1.5f;   // begin arrival-deceleration this far from destination
    float repathDistance    = 1.0f;   // destination must drift this far to trigger a fresh FindPath
    bool  rotateToFacing    = true;   // honour facingMode (set false to fully manual)
    float turnRate          = 8.0f;   // radians/sec for facing slerp

    // ===== Stuck detection (DesignMd §7 坑 2) ============================
    // NavAgentSystem accumulates stuckTimer while CCC.wasBlockedLastStep is
    // true AND the block direction opposes the steering direction. After
    // exceeding ~0.5s the system marks pathDirty so the next tick re-plans.
    float             stuckTimer        = 0.0f;
    DirectX::XMFLOAT3 lastPositionSample = { 0.0f, 0.0f, 0.0f };

    // ===== Runtime — NavAgentSystem owns these, do not edit externally ====
    std::vector<DirectX::XMFLOAT3> path;
    std::uint32_t     nextWaypoint     = 0;
    DirectX::XMFLOAT3 lastPlannedTarget = { 0.0f, 0.0f, 0.0f };
    bool              pathValid        = false;
    bool              pathDirty        = false;   // request a fresh FindPath this tick (stuck recovery, dest moved)
    NavAgentState     state            = NavAgentState::Idle;
};
