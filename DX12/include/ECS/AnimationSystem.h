#pragma once

// AnimationSystem — samples clip keyframes and writes per-entity LocalPose (scratch).
// LocalToWorldSystem — accumulates local transforms root-first into world-space matrices,
//                      writing them into PoseRingBuffer.
// SkinMatrixSystem   — multiplies world bone matrices by inverseBindPose in-place,
//                      and allocates per-entity slices in SkinnedVertexRing.
//
// Call order each frame (on the main thread, before GPU submission):
//   1. AnimationSystem::Update(world, dt)
//   2. LocalToWorldSystem::Update(world, poseRing)
//   3. SkinMatrixSystem::Update(world, poseRing, vertRing)
//
// After step 3, each skinned entity's SkinningOutputComponent holds the byte offsets
// needed by SkinningPass (GPU) and Renderer::BeginFrame (MeshDescriptor patch).

#include "ECS/ECS.h"
#include "ECS/AnimationComponents.h"
#include "Resource/SkeletonAsset.h"
#include "Graphics/SkinningBuffers.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <DirectXMath.h>

// ---------------------------------------------------------------------------
// AnimationSystem
// Reads:  AnimationComponent, SkeletonComponent
// Writes: internal LocalPose scratch (retrieved by LocalToWorldSystem)
// ---------------------------------------------------------------------------
class AnimationSystem
{
public:
    struct LocalPose
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT4 rot; // quaternion xyzw
        DirectX::XMFLOAT3 scl;
    };

    explicit AnimationSystem(SkeletonRegistry& skeletons,
                             ClipLibrary&       clips,
                             MorphClipLibrary&  morphClips)
        : m_skeletons(skeletons), m_clips(clips), m_morphClips(morphClips) {}

    // Advance all animations and sample keyframes into m_localPoseCache.
    // Applies pre-IK grants (腰キャンセル etc.). Must be called before IKSystem.
    // If activeSet != nullptr, only entities in the set are updated.
    void Update(World& world, float dt,
                const std::unordered_set<Entity>* activeSet = nullptr);

    // Apply post-IK grants (D-bones that copy IK-solved rotations).
    // Must be called AFTER IKSystem::Update() and before LocalToWorldSystem.
    void ApplyPostIKGrants(World& world);

    // Returns the LocalPose array for entity e (valid until next Update()).
    // Returns nullptr if e has no AnimationComponent / SkeletonComponent.
    const LocalPose* GetLocalPose(Entity e) const;

    // Mutable accessor for IKSystem to write back solved rotations.
    LocalPose* GetMutableLocalPose(Entity e);

private:
    void SampleClip(const ClipAsset& clip, float time,
                    uint32_t boneCount, LocalPose* out) const;

    void SampleMorphClip(const MorphClipAsset& mc, float time,
                         MorphComponent* out) const;

    SkeletonRegistry& m_skeletons;
    ClipLibrary&      m_clips;
    MorphClipLibrary& m_morphClips;

    // Per-entity scratch LocalPose storage — flat array indexed by Entity ID.
    // Avoids unordered_map hash lookups; enables thread-safe parallel access
    // (different entities write to disjoint slots).
    std::vector<std::vector<LocalPose>> m_localPoseCache; // [entityID] → LocalPose[]
    std::vector<uint32_t>               m_lastClipCache;  // [entityID] → last primaryClip
    void EnsureCacheSize(Entity e);
};

// ---------------------------------------------------------------------------
// LocalToWorldSystem
// Reads:  SkeletonComponent, SkinningOutputComponent, AnimationSystem local poses,
//         SkeletonAsset.parentIndex[]
// Writes: PoseRingBuffer (world-space bone matrices, one float4x4 per bone)
//         SkinningOutputComponent.poseByteOffset
// ---------------------------------------------------------------------------
class LocalToWorldSystem
{
public:
    explicit LocalToWorldSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    // Traverse all skinned entities root-first and write world-space bone matrices.
    // Must be called after AnimationSystem::Update().
    void Update(World& world, PoseRingBuffer& poseBuffer,
                const std::unordered_set<Entity>* activeSet = nullptr);

private:
    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;
};

// ---------------------------------------------------------------------------
// SkinMatrixSystem
// Reads:  PoseRingBuffer (world-space), SkeletonAsset.inverseBindPose[]
// Writes: PoseRingBuffer in-place (finalised skin matrices = world × inverseBindPose)
//         SkinningOutputComponent.outPosByteOffset / outNrmByteOffset
//         SkinnedVertexRing allocation heads advanced
// ---------------------------------------------------------------------------
class SkinMatrixSystem
{
public:
    explicit SkinMatrixSystem(SkeletonRegistry& skeletons)
        : m_skeletons(skeletons) {}

    // Finalise skinning matrices and allocate per-entity output slices.
    // Must be called after LocalToWorldSystem::Update().
    void Update(World& world, PoseRingBuffer& poseBuffer, SkinnedVertexRing& vertRing);

private:
    SkeletonRegistry& m_skeletons;
};
