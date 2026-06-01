#pragma once

// PlayerControllerSystem — drives every entity carrying
// PlayerComponent + CharacterControllerComponent.
//
// Per-frame pipeline:
//   1. Resolve camera basis: forward / right projected onto XZ from the
//      player's `cameraEntity` (or App's main camera if NullEntity).
//   2. Read WASD: compose a unit direction in world space.
//   3. Sprint via LShift: pick walkSpeed vs runSpeed.
//   4. Apply airControl factor while airborne.
//   5. Write CCC.desiredHorizontalVelocity.
//   6. Jump: Space + (isGrounded || timeInAir <= coyoteTime) → fire jump;
//      otherwise buffer for up to PlayerComponent.jumpBufferTime so a press
//      slightly before landing still results in a jump.
//
// Must run BEFORE PhysicsSystem::Update each frame — that's when the CCC's
// drive fields are consumed.

#include "ECS/ECS.h"   // Entity / NullEntity — used in Update's signature

class World;

class PlayerControllerSystem
{
public:
    // World ticks every player entity. @p mainCamera is the fallback basis
    // used when a PlayerComponent has `cameraEntity == NullEntity` (the App
    // already maintains "the main camera" so we don't redo that resolve
    // here). @p dt is real per-frame delta in seconds.
    void Update(World& world, Entity mainCamera, float dt);
};
