#pragma once

// FootIKTargetSystem — bridges PhysicsSystem raycasts and IKSystem's CCD
// solver so PMX-rigged characters' feet snap to the actual terrain.
//
// Call order (must be inserted exactly here):
//   1. AnimationSystem::Update         — samples clips, pre-IK grants
//   2. FootIKTargetSystem::Update      — *THIS*: raycast + write IK targets
//   3. IKSystem::Update                — CCD solver pulls effector to target
//   4. AnimationSystem::ApplyPostIKGrants
//   5. LocalToWorldSystem::Update
//
// One per-frame pass. No state across frames — the auto-detect cache lives
// on FootIKComponent itself (leftChainIdx / rightChainIdx).

#include "ECS/ECS.h"

#include <unordered_set>

class AnimationSystem;
class SkeletonRegistry;
namespace DX12Physics { class PhysicsSystem; }

class FootIKTargetSystem
{
public:
    explicit FootIKTargetSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    // Editor toggle. When disabled or `physics == nullptr`, Update is a
    // no-op — IK chains keep whatever target the animation clip baked in,
    // which is the legacy behavior pre-FootIK.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // Per-frame pass. `activeSet`, when non-null, filters to a culled set
    // (parallel to AnimationSystem / IKSystem). `physics` may be null in
    // very early init or in tools that don't spin up Jolt.
    void Update(World&                                    world,
                const DX12Physics::PhysicsSystem*         physics,
                const std::unordered_set<Entity>*         activeSet = nullptr);

private:
    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;
    bool              m_enabled = true;
};
