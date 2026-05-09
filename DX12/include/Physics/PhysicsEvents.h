#pragma once

// Events emitted by the physics layer.
//
// ContactBeganEvent is deliberately gameplay-agnostic — it's just "bodyA and
// bodyB started touching this frame". Filtering (is this a weapon hit? did a
// player step on a trigger?) is done by subscribers via TagComponent or
// component queries, not baked into the event type. This keeps the event
// bus shallow — one broadcast instead of per-gameplay-concept variants.
//
// PhysicsSystem populates this on the main thread AFTER the physics step
// has finished (Jolt contact callbacks run on worker threads, so the
// physics ContactListener buffers contacts under a mutex and drains them
// after Physics::Update returns — see PhysicsContactListener in the impl).

#include "ECS/ECS.h"

#include <DirectXMath.h>

struct ContactBeganEvent
{
    EntityHandle      bodyA;
    EntityHandle      bodyB;
    DirectX::XMFLOAT3 point;     // world-space contact point
    DirectX::XMFLOAT3 normal;    // world-space normal, pointing from bodyA → bodyB
};
