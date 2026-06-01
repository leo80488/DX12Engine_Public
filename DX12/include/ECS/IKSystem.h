#pragma once

// IKSystem — CCD-IK solver for MMD/PMX models, ported from saba (benikabocha).
//
// Call order each frame:
//   1. AnimationSystem::Update()          — samples clips, applies pre-IK grants
//   2. IKSystem::Update()                 — solves IK chains (modifies local poses)
//   3. AnimationSystem::ApplyPostIKGrants — applies post-IK grants (D-bones)
//   4. LocalToWorldSystem::Update()       — accumulates world-space bone matrices
//
// The IK solver reads the IK bone's animated position as the target,
// then adjusts the chain link bones' IK rotations so the effector
// (target bone) reaches the IK bone's position.

#include "ECS/ECS.h"
#include "ECS/AnimationSystem.h"
#include "Resource/SkeletonAsset.h"

#include <DirectXMath.h>
#include <unordered_set>
#include <vector>

class IKSystem
{
public:
    explicit IKSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    // Solve IK chains for all entities with active animation.
    void Update(World& world, const std::unordered_set<Entity>* activeSet = nullptr);

    // Editor toggle — when false, Update() early-outs and every skeleton
    // keeps its raw animated pose untouched (no IK correction). Useful for
    // A/B comparing rig authoring, debugging foot-slide, or disabling IK
    // on characters whose clips already bake the end-effector trajectory.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // Public reusable kinematics helpers — FootIKTargetSystem (and any other
    // pre-IK pass) needs to read the skeleton's pre-IK world pose to decide
    // where the effector currently sits. Both helpers are pure (no member
    // state) so they live as static methods to keep the surface tight.
    static DirectX::XMMATRIX LocalPoseToMatrix(const AnimationSystem::LocalPose& p);
    static DirectX::XMMATRIX ComputeBoneWorldTransform(
        uint32_t                          boneIndex,
        const AnimationSystem::LocalPose* poses,
        const SkeletonAsset&              skel);

private:
    // Per-chain-link runtime state used during solving.
    struct ChainState
    {
        DirectX::XMVECTOR animRot;       // original animation rotation (before IK)
        DirectX::XMVECTOR ikRot;         // accumulated IK rotation
        DirectX::XMVECTOR saveIKRot;     // best-so-far IK rotation
        DirectX::XMFLOAT3 prevAngle;     // previous Euler angles for Decompose
        float             planeModeAngle; // cumulative angle for SolvePlane
    };

    enum class SolveAxis { X, Y, Z };

    // Solve a single IK chain (outer loop with distance tracking).
    void SolveChain(const SkeletonAsset::IKChain& chain,
                    AnimationSystem::LocalPose*    poses,
                    const SkeletonAsset&           skel,
                    uint32_t                       boneCount);

    // Single CCD iteration over all chain links.
    void SolveCore(uint32_t                       iteration,
                   const SkeletonAsset::IKChain&  chain,
                   AnimationSystem::LocalPose*    poses,
                   const SkeletonAsset&           skel,
                   std::vector<ChainState>&       states);

    // Single-axis solver for knee-like joints.
    void SolvePlane(uint32_t                       iteration,
                    size_t                         chainIdx,
                    SolveAxis                      axis,
                    const SkeletonAsset::IKChain&  chain,
                    AnimationSystem::LocalPose*    poses,
                    const SkeletonAsset&           skel,
                    std::vector<ChainState>&       states);

    // Compute world-space transform for a bone (traverses parent chain).
    // Member alias of the public static — keeps the existing solver call
    // sites unchanged.
    DirectX::XMMATRIX ComputeWorldTransform(
        uint32_t                          boneIndex,
        const AnimationSystem::LocalPose* poses,
        const SkeletonAsset&              skel) const
    {
        return ComputeBoneWorldTransform(boneIndex, poses, skel);
    }

    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;
    bool              m_enabled = true;
};
