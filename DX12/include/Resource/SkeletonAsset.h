#pragma once

// SkeletonAsset  — bone hierarchy + bind pose (immutable after import, shared across entities)
// ClipAsset      — animation keyframe data SOA layout (immutable after import)
// BlendVertex    — per-vertex bone influences packed in 8 bytes (immutable after mesh upload)
// SkeletonRegistry / ClipLibrary — global registries accessed by uint32_t index
//
// DESIGN RULE: Bones are NEVER ECS entities. They are uint32_t indices inside SkeletonAsset.
// parentIndex[i] < i for all i > 0  (root-first / topological order)
// This invariant guarantees dependency-free traversal in all systems.

#include "ECS/NotifyTypes.h"  // NotifyTrack — clip-authored AnimNotify data (Unreal-style)

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>

static constexpr uint32_t kInvalidSkeletonIndex = ~0u;
static constexpr uint32_t kInvalidClipIndex     = ~0u;

// ===========================================================================
// BlendVertex — 24 bytes per mesh vertex; one per vertex in the mesh
// boneIndices[i] = 0-based bone index in SkeletonAsset (uint16, max 65535)
// boneWeights[i] = normalised weight, 255 = 1.0
// Supports up to 8 bone influences per vertex, up to 65535 bones.
// ===========================================================================
static constexpr uint32_t MAX_BONES_PER_VERTEX = 8;

struct BlendVertex
{
    uint16_t boneIndices[MAX_BONES_PER_VERTEX]; // 16 bytes
    uint8_t  boneWeights[MAX_BONES_PER_VERTEX]; // 8 bytes
};
static_assert(sizeof(BlendVertex) == 24, "BlendVertex must be exactly 24 bytes");

// ===========================================================================
// SkeletonAsset — load-time data, immutable, shared by all entities using
//                 the same skeleton. Stored in SkeletonRegistry by index.
// ===========================================================================
struct SkeletonAsset
{
    static constexpr uint32_t MAX_BONES = 1024;

    uint32_t boneCount = 0;

    // Bone topology — root-first topological order baked at import time.
    // parentIndex[i] = parent bone index (-1 for root).
    // INVARIANT: parentIndex[i] < i  (i > 0)
    int32_t parentIndex[MAX_BONES]{};

    // Bind pose matrices
    DirectX::XMFLOAT4X4 inverseBindPose[MAX_BONES]; // model-space -> bone-space
    DirectX::XMFLOAT4X4 bindPose[MAX_BONES];        // bone-space  -> model-space (for SocketSystem)
    DirectX::XMFLOAT4X4 restPoseLocal[MAX_BONES];   // local TRS at rest pose

    // Bone names — used only for import-time lookups and socket binding.
    // NOT read at runtime by AnimationSystem or GPU skinning.
    char boneNames[MAX_BONES][64]{};

    // Hash map: FNV-32 of bone name -> bone index
    // Populated at import time; used to resolve bone name -> index lookups.
    std::unordered_map<uint32_t, uint32_t> nameToIndex;

    // Per-bone rest-pose AABB (bone-local space) — for skinned mesh AABB proxy.
    // Computed at import time from vertex influence weights transformed by inverseBindPose.
    struct BoneAABB
    {
        DirectX::XMFLOAT3 localMin = {  1e30f,  1e30f,  1e30f };
        DirectX::XMFLOAT3 localMax = { -1e30f, -1e30f, -1e30f };
    };
    BoneAABB boneRestAABBs[MAX_BONES]{};
    bool     hasBoneAABBs = false;  // true after import computes per-bone AABBs

    // PMX grant (付与) system — D-bones copy rotation from a source bone.
    // grantSource[i] = source bone index to copy rotation from (-1 = no grant).
    // grantRatio[i]  = blend ratio (typically 1.0 for D-bones).
    // Populated at import time from PMX binary data or by name-matching heuristic.
    int32_t grantSource[MAX_BONES]{};
    float   grantRatio[MAX_BONES]{};

    // PMX transform order — controls evaluation order for grants and IK.
    // Lower values are evaluated first.  Populated from PMX bone data.
    int32_t transformOrder[MAX_BONES]{};

    // PMX IK chain definitions — parsed directly from PMX bone data.
    struct IKLink
    {
        uint32_t            boneIndex      = 0;
        bool                hasAngleLimit  = false;
        DirectX::XMFLOAT3  minAngle       = { 0.f, 0.f, 0.f }; // radians
        DirectX::XMFLOAT3  maxAngle       = { 0.f, 0.f, 0.f }; // radians
    };
    struct IKChain
    {
        uint32_t            ikBoneIndex     = 0;   // the IK bone (e.g. 左足ＩＫ)
        uint32_t            targetBoneIndex = 0;   // effector (e.g. 左足首)
        uint32_t            loopCount       = 0;   // CCD iterations
        float               angleLimit      = 0.f; // per-iteration clamp (radians)
        std::vector<IKLink> links;                  // chain from effector toward root
    };
    std::vector<IKChain> ikChains;

    // Morph target names — ordered to match the morph delta buffer layout.
    // Used for runtime name-matching against MorphClipAsset channel names.
    static constexpr uint32_t MAX_MORPH_TARGETS = 128;
    uint32_t morphTargetCount = 0;
    char morphTargetNames[MAX_MORPH_TARGETS][64]{};

    // Socket definitions — named attachment points for weapons, particles, etc.
    struct Socket
    {
        uint32_t            boneIndex  = 0;
        DirectX::XMFLOAT4X4 localOffset;    // offset relative to bone world transform
        char                name[64]{};
        uint32_t            nameHash   = 0; // FNV-32 of name
    };
    std::vector<Socket> sockets;
};

// ===========================================================================
// ClipAsset — animation keyframes in SOA layout.
// Index: boneIdx * frameCount + frameIdx
// ===========================================================================
struct ClipAsset
{
    uint32_t boneCount  = 0;
    uint32_t frameCount = 0;
    float    duration   = 0.f; // total clip length in seconds
    float    frameRate  = 30.f;

    // SOA keyframe arrays: [boneIdx * frameCount + frameIdx]
    std::vector<DirectX::XMFLOAT3> positions;  // boneCount × frameCount
    std::vector<DirectX::XMFLOAT4> rotations;  // boneCount × frameCount (quaternion xyzw)
    std::vector<DirectX::XMFLOAT3> scales;     // boneCount × frameCount

    struct AnimEvent
    {
        float    time;
        uint32_t nameHash;
    };
    std::vector<AnimEvent> events;

    // ---- AnimNotify tracks (Unreal AnimSequence-style) ---------------------
    // Authored on the clip asset and carried into the runtime clip by
    // AnimClipData::BindToSkeleton. TimelineSystem fires these against
    // AnimationComponent::primaryTime — see ECS/TimelineSystem.h. Notifies are
    // time-based and skeleton-independent, so a single authored set drives
    // every entity that plays this clip (no per-entity authoring required).
    std::vector<NotifyTrack> notifyTracks;
    uint32_t                 nextNotifyId = 1; // editor id allocator, persisted with the clip
};

// ===========================================================================
// MorphClipAsset — runtime morph (blend-shape) animation clip, SOA layout.
// Weights are in [0, 1].  Index: morphIdx * frameCount + frameIdx.
// morphNames[i] must match the mesh morph target name for binding.
// ===========================================================================
struct MorphClipAsset
{
    static constexpr uint32_t MAX_MORPHS = 128;

    uint32_t morphCount = 0;
    uint32_t frameCount = 0;
    float    duration   = 0.f;
    float    frameRate  = 30.f;

    char morphNames[MAX_MORPHS][64]{};  // morph target name per channel

    // SOA weight data: weights[morphIdx * frameCount + frameIdx]
    std::vector<float> weights;
};

// ===========================================================================
// MorphClipLibrary — owns all loaded MorphClipAssets; accessed by uint32_t index.
// ===========================================================================
class MorphClipLibrary
{
public:
    uint32_t              Register(MorphClipAsset clip);
    const MorphClipAsset& Get(uint32_t index) const;
    uint32_t              Count() const { return static_cast<uint32_t>(m_clips.size()); }

private:
    std::vector<MorphClipAsset> m_clips;
};

// ===========================================================================
// SkeletonRegistry — owns all loaded SkeletonAssets; accessed by uint32_t index.
// Thread-safety: single-threaded; registrations happen at load time only.
// ===========================================================================
class SkeletonRegistry
{
public:
    // Register a new SkeletonAsset; returns stable index (never invalidated).
    uint32_t       Register(SkeletonAsset asset);

    SkeletonAsset& Get(uint32_t index);
    const SkeletonAsset& Get(uint32_t index) const;

    uint32_t Count() const { return static_cast<uint32_t>(m_skeletons.size()); }

private:
    std::vector<SkeletonAsset> m_skeletons;
};

// ===========================================================================
// ClipLibrary — owns all loaded ClipAssets; accessed by uint32_t index.
// ===========================================================================
class ClipLibrary
{
public:
    uint32_t         Register(ClipAsset clip);
    const ClipAsset& Get(uint32_t index) const;
    // Mutable access — used by the animation editor to live-update a bound
    // clip's notify tracks during preview so "editor sees == game sees".
    // Caller must ensure index < Count(). Do NOT mutate the SOA keyframe
    // arrays at runtime (AnimationSystem reads them every frame).
    ClipAsset&       GetMutable(uint32_t index) { assert(index < m_clips.size()); return m_clips[index]; }
    uint32_t         Count() const { return static_cast<uint32_t>(m_clips.size()); }

private:
    std::vector<ClipAsset> m_clips;
};

// ===========================================================================
// Inline implementations
// ===========================================================================
inline uint32_t SkeletonRegistry::Register(SkeletonAsset asset)
{
    uint32_t idx = static_cast<uint32_t>(m_skeletons.size());
    m_skeletons.push_back(std::move(asset));
    return idx;
}
inline SkeletonAsset& SkeletonRegistry::Get(uint32_t index)
{
    return m_skeletons[index];
}
inline const SkeletonAsset& SkeletonRegistry::Get(uint32_t index) const
{
    return m_skeletons[index];
}

inline uint32_t ClipLibrary::Register(ClipAsset clip)
{
    uint32_t idx = static_cast<uint32_t>(m_clips.size());
    m_clips.push_back(std::move(clip));
    return idx;
}
inline const ClipAsset& ClipLibrary::Get(uint32_t index) const
{
    return m_clips[index];
}

inline uint32_t MorphClipLibrary::Register(MorphClipAsset clip)
{
    uint32_t idx = static_cast<uint32_t>(m_clips.size());
    m_clips.push_back(std::move(clip));
    return idx;
}
inline const MorphClipAsset& MorphClipLibrary::Get(uint32_t index) const
{
    return m_clips[index];
}
