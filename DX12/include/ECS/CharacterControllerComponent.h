#pragma once

// CharacterControllerComponent — kinematic character controller (KCC).
// The *execution-layer Motor* in the layered movement architecture
// (DesignMd/character_movement_architecture.md §1):
//   AIIntentComponent (strategy)
//        ↓
//   NavAgentComponent (tactical — destination + path)
//        ↓
//   CharacterControllerComponent (execution — what THIS file is)
//        ↓
//   LocalTransform (after Jolt sweep/slide/ground snap)
//
// Independent of RigidBodyComponent / ColliderComponent: an entity with this
// component is driven by JPH::CharacterVirtual inside PhysicsSystem, NOT by
// the rigid-body path. The two cannot coexist on the same entity; if both are
// present PhysicsSystem prefers the KCC and ignores the rigid body.
//
// Authoring model:
//   • Caller (PlayerControllerSystem, NavAgentSystem, CutsceneSystem) writes
//     `desiredHorizontalVelocity` (world-space XZ) and optionally toggles
//     `jumpRequested`. The Motor doesn't care whether the writer is a human
//     or an AI — same code path for all (Motor invariant, §1.2).
//   • PhysicsSystem applies gameplay gravity to `verticalVelocity` and feeds
//     both into JPH::CharacterVirtual::ExtendedUpdate each fixed step.
//   • After the step, PhysicsSystem writes back `velocity`, `isGrounded`,
//     `groundNormal`, `mode`, `wasBlockedLastStep`, etc. for the caller to
//     consume next frame.
//
// Gravity: deliberately separate from Jolt's PhysicsSystem gravity. Gameplay
// gravity (-20 to -30 m/s²) gives a snappier "video game" feel than real
// -9.8 m/s². ExtendedUpdate is called with JPH::Vec3::sZero() — the KCC
// handles vertical integration itself.

#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>

inline constexpr std::uint32_t kInvalidCharacterId = 0xffffffffu;

// Locomotion mode — Motor maintains this based on grounded state, water
// volumes, and off-mesh-link traversal (§2.1, §7 off-mesh link handshake).
enum class MovementMode : std::uint8_t
{
    Walking,            // standing on a walkable slope
    Falling,            // airborne (jump apex / coyote / off ledge)
    Swimming,           // inside a water volume (gravity off, drag on)
    Climbing,           // on a ladder / climb surface
    LaunchedTraversal,  // following an off-mesh-link arc; Motor ignores desiredHorizontalVelocity
};

struct CharacterControllerComponent : ComponentBase
{
    // ---- Capsule shape ----
    // Default: 1.7m total height capsule (0.7 × 2 = 1.4 cylinder + 0.3 × 2
    // hemispheres = 2.0m, but capsule height = cylinder + 2*radius = 1.4 + 0.6
    // = 2.0m). Adjust radius/halfHeight to match the character's silhouette
    // — but keep it a single capsule. Splitting into multiple shapes for
    // movement breaks collide-and-slide; per-bone collision belongs to a
    // separate hurt-box layer.
    float capsuleRadius     = 0.30f;
    float capsuleHalfHeight = 0.70f;  // central segment, EXCLUDES hemispheres

    // ---- Movement tuning ----
    // Maximum step (in metres) the character can ascend by step-offset. Below
    // this height a small ledge is treated as walkable terrain (the KCC
    // virtually lifts then drops); above it the ledge becomes a wall.
    float stepHeight        = 0.30f;
    // Slopes shallower than this are walkable (gravity cancels, motion is
    // projected along the slope). Steeper slopes accumulate gravity →
    // character slides down. Default 45° matches platformer convention.
    float maxSlopeRad       = 0.7854f;
    // Tiny inset between the capsule and the world to keep narrow-phase
    // numerically stable. Don't push to 0 — sliding contacts will visibly
    // jitter.
    float skinWidth         = 0.02f;
    // Mass used when pushing dynamic props. CharacterVirtual is not a rigid
    // body, so this only affects impulses it injects into things it bumps.
    float mass              = 80.f;
    // Strength multiplier on impulses applied to dynamic objects the
    // character runs into. 0 = no push, 1 = realistic kinematic push.
    float pushStrength      = 1.0f;

    // ---- Gameplay gravity (NOT Jolt PhysicsSystem gravity) ----
    // m/s². Negative pulls down. Larger magnitude = snappier feel; the
    // canonical 3D-platformer range is -20 to -30. Real-world -9.8 makes
    // jumps feel floaty.
    float gravity           = -25.0f;
    // Terminal fall speed (clamp on |verticalVelocity|). Prevents
    // teleporting through thin floors after long falls; also clamps the
    // worst-case ground-probe overshoot.
    float maxFallSpeed      = 55.f;

    // ---- Drive inputs (caller writes each frame) ----
    // World-space horizontal velocity in m/s. The Y component is ignored —
    // vertical motion is owned by the KCC (gravity + jump). Caller computes
    // this from input (e.g. forward * speed + strafe * sideSpeed) or
    // pathfinding steering (NavAgentSystem).
    DirectX::XMFLOAT3 desiredHorizontalVelocity = { 0.f, 0.f, 0.f };
    // Desired world-space facing direction (XZ only; Y ignored). When
    // useDesiredFacing is true, callers communicate "this is what I want
    // to face" via this field; the active facing-applier (currently
    // NavAgentSystem / PlayerControllerSystem write LT.rotation directly,
    // future migration target is a unified FacingSystem reading this) uses
    // it as the slerp target. Anyone — input, AI, cutscene — can write it.
    DirectX::XMFLOAT3 desiredFacing     = { 0.f, 0.f, 1.f };
    bool              useDesiredFacing  = false;
    // Set true on the frame the caller wants the character to leap. Consumed
    // (set back to false) by PhysicsSystem after applying jumpSpeed to the
    // vertical velocity. Only honoured when `isGrounded` is true. For better
    // input feel use the buffer pattern from PlayerComponent rather than
    // setting this directly — see DesignMd §7 坑 3.
    bool              jumpRequested = false;
    // Crouch toggle. KCC step currently stores only; future work shrinks the
    // capsule + lowers max walk speed while held (DesignMd §2.1).
    bool              wantsCrouch   = false;
    // Instantaneous vertical velocity applied on jump (m/s). Used directly;
    // KCC does NOT solve for jump height — designer picks the speed so the
    // resulting apex matches the level layout.
    float             jumpSpeed     = 7.5f;

    // ---- Escape hatch (cutscene / teleport) -------------------------------
    // When set, the next KCC step zeroes velocity + desiredHorizontalVelocity
    // and immediately clears the flag. Prevents the inertia-glide that
    // happens when a cutscene just writes desiredHorizontalVelocity = 0
    // (DesignMd §7 坑 10).
    bool              instantStopRequested = false;

    // ---- Off-mesh link handshake (DesignMd §7 坑 6) ----------------------
    // NavAgentSystem sets `mode = LaunchedTraversal` and pre-loads
    // `velocity` to the ballistic launch vector when an off-mesh-link
    // (jump down / jump across) is entered. Motor lets the projectile fly
    // (ignores desiredHorizontalVelocity in this mode), and on landing
    // (isGrounded transitions true) sets `launchFinished = true` so the
    // NavAgent can resume path-following. Currently unused (Recast off-
    // mesh-link support is future work) but the field is here so the
    // Motor's existing skeleton can be extended without touching callers.
    bool              launchFinished = false;

    // ---- State (PhysicsSystem writes each fixed step) ----
    // Current world velocity (XYZ). Caller may read for animation blending,
    // dust VFX, etc. Do NOT write — PhysicsSystem overwrites every step.
    DirectX::XMFLOAT3 velocity      = { 0.f, 0.f, 0.f };
    // Surface contact state. `isGrounded` true means the KCC is standing on
    // a walkable slope; airborne or on a too-steep slope returns false.
    bool              isGrounded    = false;
    bool              wasGrounded   = false;  // previous frame — for landing/takeoff events
    // World-space normal of the surface the character is standing on.
    // Defaults to +Y when airborne so downstream code doesn't need a
    // null-check guard.
    DirectX::XMFLOAT3 groundNormal  = { 0.f, 1.f, 0.f };
    // Entity ID of the body the character is standing on, or NullEntity
    // when airborne / not attached. Useful for moving-platform parenting,
    // dynamic-prop interactions, surface-material lookup.
    Entity            groundEntity  = NullEntity;
    // Seconds the character has been off the ground. Used by gameplay code
    // for coyote time (still allow jump within 0.1s of takeoff) and air-
    // duration animation cues.
    float             timeInAir     = 0.f;

    // Current locomotion mode. Set by KCC step from grounded state +
    // volume queries + LaunchedTraversal handshake (see launchFinished
    // above). AnimationSystem reads this to pick locomotion blends.
    MovementMode      mode          = MovementMode::Falling;

    // ---- Motor reports back to upstream (read by NavAgent for stuck-
    //      detection / replan trigger, DesignMd §7 坑 2) -------------------
    // True when the last sweep produced near-zero displacement despite a
    // non-zero desiredHorizontalVelocity — wall hit, geometry pin, etc.
    bool              wasBlockedLastStep = false;
    // Normal of the surface that blocked us (most-opposed wall contact).
    // Valid only when wasBlockedLastStep is true.
    DirectX::XMFLOAT3 blockedNormal      = { 0.f, 0.f, 0.f };

    // ---- Runtime — PhysicsSystem owns lifecycle ----
    // JPH::BodyID of the optional inner body. CharacterVirtual creates one
    // when configured, used so other physics queries can ignore the
    // character itself. kInvalidPhysicsBodyId when no inner body exists.
    std::uint32_t bodyId       = 0xffffffffu;
    // Generation counter — bump via MarkDirty() after changing capsule
    // dimensions to force PhysicsSystem to rebuild the CharacterVirtual on
    // the next step.
    std::uint32_t generation   = 0;
    // Generation the active CharacterVirtual was built from. Internal —
    // do not write.
    std::uint32_t lastBuiltGeneration = 0;

    void MarkDirty() { ++generation; }
};
