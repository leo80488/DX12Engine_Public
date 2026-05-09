#pragma once

// Physics components — plain data only, Jolt types never leak here.
// PhysicsSystem reads these on first sight to build the matching Jolt body;
// on subsequent frames it writes LocalTransform back from simulation.

#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>

// JPH::BodyID::cInvalidBodyID is 0xffffffff. We mirror that sentinel here so
// the header stays Jolt-free.
inline constexpr std::uint32_t kInvalidPhysicsBodyId = 0xffffffffu;

struct RigidBodyComponent : ComponentBase
{
    enum class Motion : std::uint8_t
    {
        Static,     // never moves; no mass
        Kinematic,  // moved by code, pushes dynamics
        Dynamic,    // full simulation (gravity, impulses, contact response)
    };

    Motion motion          = Motion::Dynamic;
    float  mass            = 1.0f;   // kg, used only for Dynamic
    float  linearDamping   = 0.05f;
    float  angularDamping  = 0.05f;
    float  friction        = 0.5f;
    float  restitution     = 0.0f;
    float  gravityFactor   = 1.0f;

    // Runtime — populated by PhysicsSystem on creation. Do not edit.
    std::uint32_t bodyId = kInvalidPhysicsBodyId;
};

struct ColliderComponent : ComponentBase
{
    enum class Shape : std::uint8_t { Box, Sphere, Capsule };

    Shape             shape       = Shape::Box;
    DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f }; // Box
    float             radius      = 0.5f;                  // Sphere / Capsule
    float             halfHeight  = 0.5f;                  // Capsule cylinder half-height (excl. hemispheres)
};
