#pragma once

// PlayerComponent — tag + tuning data for the player avatar.
//
// Pair with CharacterControllerComponent on the same entity. PlayerControllerSystem
// reads Input + this component + the basis camera's facing, then writes the
// drive fields on the CCC (desiredHorizontalVelocity / jumpRequested).
//
// Key bindings are intentionally hard-coded in PlayerControllerSystem (WASD +
// Space + LShift) — adding remap fields here is busy-work until we have a
// real input-binding layer. The system has a single VK lookup per frame so
// changing them is a one-line edit.

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

struct PlayerComponent : ComponentBase
{
    // ---- Movement feel ----
    float walkSpeed = 4.5f;     // m/s, default
    float runSpeed  = 7.5f;     // m/s, while LShift held
    // Multiplier on horizontal input while airborne. 1.0 = full control,
    // 0.0 = mid-air locked horizontal motion. 0.4 is a common 3D-platformer
    // default — tunable but not eliminated air control.
    float airControl = 0.4f;

    // ---- Facing ----
    // Angular speed at which the player avatar rotates to face its movement
    // direction (rad/s). 0 = never rotate (let animation / script own
    // rotation). Default ~12 rad/s ≈ ~688°/s — enough that direction flips
    // settle within a frame or two but not so snappy it looks like a teleport.
    // Below `facingMinSpeed` the rotation is skipped so a stationary player
    // keeps the last-faced direction.
    float turnRate       = 12.f;
    float facingMinSpeed = 0.1f;   // m/s

    // ---- Jump assist ----
    // Coyote time: still allow jump within N seconds of leaving ground.
    // The CCC tracks `timeInAir`; PlayerControllerSystem honours jump as long
    // as `timeInAir <= coyoteTime` even though `isGrounded` is already false.
    float coyoteTime     = 0.10f;
    // Jump buffer: if the player pressed Space up to N seconds BEFORE
    // landing, queue the jump and execute it on the first grounded frame.
    // Prevents the "I pressed Space right before landing and nothing
    // happened" feel. Uses `jumpBufferTimer` below.
    float jumpBufferTime = 0.10f;

    // ---- Camera basis ----
    // Entity whose GlobalTransform forward/right are projected to XZ and used
    // as the WASD movement basis. Empty ref = "use the main camera" (App
    // resolves via the first CameraComponent-bearing entity each frame).
    // Save-stable via GUID — survives scene reload.
    AttachmentRef cameraEntity;

    // ---- Runtime ----
    // Down-counter (seconds remaining) on a buffered jump. Set to
    // `jumpBufferTime` when Space is pressed; decays each frame; consumed
    // (set to 0) on the frame the jump actually fires.
    float jumpBufferTimer = 0.f;
};
