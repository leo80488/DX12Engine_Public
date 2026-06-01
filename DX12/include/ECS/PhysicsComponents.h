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
#include <string>

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

    // Per-axis lock for Dynamic bodies. Bit set = axis is frozen (zero
    // velocity / zero angular velocity on that axis); matches Jolt's
    // EAllowedDOFs but inverted (we store "what's locked", Jolt wants "what's
    // allowed") because authoring intent reads more naturally as "freeze X".
    // Stored as one byte instead of six bools so the inspector and serializer
    // round-trip a single integer.
    enum AxisLock : std::uint8_t
    {
        LockTranslationX = 1 << 0,
        LockTranslationY = 1 << 1,
        LockTranslationZ = 1 << 2,
        LockRotationX    = 1 << 3,
        LockRotationY    = 1 << 4,
        LockRotationZ    = 1 << 5,
        LockTranslationAll = LockTranslationX | LockTranslationY | LockTranslationZ,
        LockRotationAll    = LockRotationX    | LockRotationY    | LockRotationZ,
    };

    Motion motion          = Motion::Dynamic;
    float  mass            = 1.0f;   // kg, used only for Dynamic
    float  linearDamping   = 0.05f;
    float  angularDamping  = 0.05f;
    float  friction        = 0.5f;
    float  restitution     = 0.0f;
    float  gravityFactor   = 1.0f;
    std::uint8_t lockedAxes = 0;     // bitmask of AxisLock values; 0 = full 6-DOF

    bool IsAxisLocked(AxisLock a) const { return (lockedAxes & a) != 0; }
    void SetAxisLocked(AxisLock a, bool v)
    {
        if (v) lockedAxes |= a; else lockedAxes = static_cast<std::uint8_t>(lockedAxes & ~a);
    }

    // Runtime — populated by PhysicsSystem on creation. Do not edit.
    std::uint32_t bodyId = kInvalidPhysicsBodyId;
    // Generation of the ColliderComponent the active body was built from.
    // PhysicsSystem destroys + rebuilds the body when this falls behind
    // ColliderComponent::generation (either bumped explicitly via MarkDirty
    // or detected by per-frame snapshot diff). Runtime-only; not serialized.
    std::uint32_t lastBuiltGeneration = 0;
    // lockedAxes value the active body was built from. Jolt locks axes at
    // body-creation time (via MotionProperties::SetMassProperties combined
    // with EAllowedDOFs) and has no public BodyInterface call to change it,
    // so the runtime-swap detector destroys + rebuilds when this drifts.
    std::uint8_t  lastBuiltLockedAxes  = 0;
};

struct ColliderComponent : ComponentBase
{
    enum class Shape : std::uint8_t { Box, Sphere, Capsule, Mesh };

    Shape             shape       = Shape::Box;
    DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f }; // Box
    float             radius      = 0.5f;                  // Sphere / Capsule
    float             halfHeight  = 0.5f;                  // Capsule cylinder half-height (excl. hemispheres)

    // Mesh shape source. Only used when shape == Mesh. PhysicsSystem reads
    // the .meshlib file on first body creation and builds a JPH::MeshShape;
    // results are cached so multiple entities sharing the same (path, meshId)
    // reuse one Jolt shape. Mesh colliders are usable only with Static
    // RigidBodies — Jolt's MeshShape is not valid for dynamic motion.
    std::string   meshLibPath;
    std::uint32_t meshLibMeshId = 0;

    // Local-space translation applied to the collision shape relative to the
    // entity's transform. Jolt's primitive shapes are centered at origin, so
    // a capsule on an entity whose pivot sits at the character's feet ends
    // up half-buried in the floor. Set offset.y = halfHeight + radius (for a
    // capsule) — or use the "Snap to Bottom" preset in the inspector — to
    // lift the collider so its bottom touches the pivot. Implementation
    // wraps the primitive in JPH::RotatedTranslatedShape when offset != 0
    // or rotation != identity.
    DirectX::XMFLOAT3 offset = { 0.f, 0.f, 0.f };

    // Local-space rotation applied to the collision shape (XYZ Euler in
    // degrees). Typical use: rotate a capsule onto its side, or tilt a box
    // to match an angled mesh. Same authoring contract as FollowSocket /
    // FollowEntity — Euler degrees rather than quat so the inspector stays
    // jitter-free near gimbal lock; PhysicsSystem converts to JPH::Quat at
    // body-build time inside the RotatedTranslatedShape wrap.
    DirectX::XMFLOAT3 rotationEulerDeg = { 0.f, 0.f, 0.f };

    // Bumped whenever the collider definition changes in a way that requires
    // rebuilding the Jolt body (shape enum, dimensions, mesh source).
    // PhysicsSystem also performs a per-frame snapshot diff to catch direct
    // field writes (e.g. from the Inspector reflection layer), so scripts may
    // either call MarkDirty() explicitly or simply assign fields and let the
    // diff catch it. Runtime-only; not serialized.
    std::uint32_t generation = 0;

    void MarkDirty() { ++generation; }
};
