#pragma once

// ECS components for skeleton animation.
// All components live on the character entity — bones are NOT ECS entities.
//
// Layout:
//   SkeletonComponent      — asset reference (immutable after spawn)
//   AnimationComponent     — playback state (updated every frame)
//   MeshSkinnedComponent   — GPU buffer handles for rest-pose data (immutable after mesh upload)
//   SkinningOutputComponent — byte offsets into per-frame ring buffers (written by AnimationSystem)
//   SocketTransformsComponent — world-space attachment points (written by SocketSystem)

#include "ECS/ECS.h"
#include "Graphics/GraphicsStruct.h"  // RHI::GPUBuffer

#include <cstdint>
#include <cstring>
#include <DirectXMath.h>

static constexpr uint32_t kInvalidAnimHandle = ~0u;

// ---------------------------------------------------------------------------
// SkeletonComponent — identifies which SkeletonAsset this entity uses.
// Placed on the CHARACTER ROOT entity (not on mesh entities).
// ---------------------------------------------------------------------------
struct SkeletonComponent
{
    uint32_t assetIndex = kInvalidAnimHandle; // -> SkeletonRegistry::Get(assetIndex)
    uint32_t boneCount  = 0;                  // cached from SkeletonAsset

    // Per-frame mutable: byte offset into PoseRingBuffer for this skeleton's
    // finalized skin matrices.  Written by LocalToWorldSystem each frame.
    uint32_t poseByteOffset = 0;
};

// ---------------------------------------------------------------------------
// SkeletonRef — placed on MESH entities to link them to the character root
// entity that owns SkeletonComponent + AnimationComponent.
// Allows one skeleton to drive multiple mesh entities (body, clothes, hair).
// ---------------------------------------------------------------------------
struct SkeletonRef
{
    Entity entity = NullEntity; // entity holding SkeletonComponent + AnimationComponent
};

// ---------------------------------------------------------------------------
// AnimationComponent — current playback state. Mutated by AnimationSystem each frame.
// ---------------------------------------------------------------------------
struct AnimationComponent
{
    uint32_t primaryClip   = kInvalidAnimHandle; // -> ClipLibrary
    uint32_t secondaryClip = kInvalidAnimHandle; // kInvalidAnimHandle = no cross-fade active

    float primaryTime   = 0.f;    // playback position in seconds
    float secondaryTime = 0.f;
    float blendWeight   = 0.f;    // 0 = primary only, 1 = secondary only
    float speed         = 1.f;    // playback rate multiplier (negative = reverse)

    bool looping = true;
    bool paused  = false;
    uint8_t _pad[2]{};

    uint32_t pendingEventMask = 0; // bitmask of AnimEvents fired this frame (consumed by game)

    // ---- Clip-driven AnimNotify cursor (Unreal AnimSequence-style) ---------
    // TimelineSystem advances these to detect which notify times the clip
    // crossed since last frame. prevNotifyTime < 0 means uninitialized: on
    // first sight (or after a clip switch) TimelineSystem snapshots the time
    // without firing, so opening a clip doesn't replay every notify at once.
    // lastNotifyClip detects primaryClip changes to re-arm the snapshot.
    float    prevNotifyTime = -1.f;
    uint32_t lastNotifyClip = kInvalidAnimHandle;
};

// ---------------------------------------------------------------------------
// MeshSkinnedComponent — GPU buffer handles for a skinned mesh.
// Immutable after mesh upload. Holds everything MeshComponent holds, plus
// the skinning-specific rest-pose and blend-weight streams.
//
// Static streams (uploaded once at registration):
//   restPosBuffer  : StructuredBuffer<float3>, 12 B/vertex — rest-pose positions
//   restNrmBuffer  : StructuredBuffer<float3>, 12 B/vertex — rest-pose normals
//   blendBuffer    : ByteAddressBuffer, BlendVertex[] — 8 B/vertex
//   uvBuffer       : StructuredBuffer<float2>, 8 B/vertex — UV set 0
//   tangentBuffer  : StructuredBuffer<float4>, 16 B/vertex — tangents (optional)
//   indexBuffer    : ByteAddressBuffer, uint32[] — triangle indices
//
// Per-frame skinned output is written into SkinnedVertexRing by the GPU
// compute pass and referenced via the skinned MeshDescriptor.
// ---------------------------------------------------------------------------
struct MeshSkinnedComponent
{
    // --- Rest-pose + blend buffers (static, immutable after upload) ---------
    RHI::GPUBuffer blendBuffer;      // 8 B/vertex:  BlendVertex (indices + weights)
    RHI::GPUBuffer restPosBuffer;    // 12 B/vertex: float3 rest-pose positions
    RHI::GPUBuffer restNrmBuffer;    // 12 B/vertex: float3 rest-pose normals

    // --- Static mesh streams (UV, tangent, index — like MeshComponent) -----
    RHI::GPUBuffer uvBuffer;         // 8 B/vertex:  float2 UV set 0
    RHI::GPUBuffer tangentBuffer;    // 16 B/vertex: float4 tangent (may be invalid if absent)
    RHI::GPUBuffer indexBuffer;      // 4 B/index:   uint32 triangle indices

    // --- Counts (like MeshComponent) ----------------------------------------
    uint32_t vertexCount        = 0;
    uint32_t indexCount         = 0;

    // --- Mesh descriptor tracking -------------------------------------------
    uint32_t meshDescriptorIdx  = ~0u; // stable slot in MeshDescriptorHeap

    // Copy of the base MeshDescriptor used for patching (cached at registration time).
    // UV/index/tangent streams come from here; pos/nrm are overridden per frame.
    RHI::MeshDescriptor baseMeshDesc{};

    // --- Bounding volume (like MeshComponent.aabb) --------------------------
    DirectX::XMFLOAT3 aabbMin = {  0.f,  0.f,  0.f };
    DirectX::XMFLOAT3 aabbMax = {  0.f,  0.f,  0.f };

    // --- Per-mesh bone influence list (for per-mesh bone AABB merge) ---------
    // Unique bone indices that have non-zero weight on at least one vertex of
    // this mesh. Populated at RegisterSkinnedMesh time from BlendVertex data.
    // Used by Renderer::BeginFrame to compute a tight AABB per mesh instead of
    // broadcasting the full-skeleton AABB to all children.
    std::vector<uint16_t> influenceBones;

    // --- Cached GPU descriptor handles (SRV) --------------------------------
    // Set by Renderer after buffer creation; used directly in SkinDispatchDesc.
    uint64_t blendSRVHandle   = 0;
    uint64_t restPosSRVHandle = 0;
    uint64_t restNrmSRVHandle = 0;

    // --- Morph targets (vertex morphs) --------------------------------------
    // Dense layout: float3 deltas[morphCount * vertexCount]
    // Index: morphIdx * vertexCount + vertexIdx
    RHI::GPUBuffer morphDeltaBuffer;
    uint64_t       morphDeltaSRVHandle = 0;
    uint32_t       morphTargetCount    = 0;
};

// ---------------------------------------------------------------------------
// SkinningOutputComponent — per-frame allocation results written by AnimationSystem.
// Read by SkinningPass (GPU dispatch) and Renderer::BeginFrame (MeshDescriptor patch).
// ---------------------------------------------------------------------------
struct SkinningOutputComponent
{
    // ~0u = sentinel: no valid pose this frame (animation not active or not yet computed).
    uint32_t poseByteOffset       = ~0u; // byte offset into PoseRingBuffer for this entity's bones
    uint32_t outPosByteOffset     = 0;   // byte offset into SkinnedVertexRing position buffer
    uint32_t outNrmByteOffset     = 0;   // byte offset into SkinnedVertexRing normal buffer
    uint32_t prevPosElementBase   = ~0u; // TAA: base element index for prev-frame skinned pos (0xFFFFFFFF = none)
};

// ---------------------------------------------------------------------------
// MorphComponent — current morph/blend-shape weights for one entity.
//
// primaryMorphClip → index into MorphClipLibrary (set by AnimationClipSystem).
// weights[]        → live output written by AnimationSystem each frame.
//                    Parallel to MorphClipAsset::morphNames[].
//                    The GPU skinning/morph pass reads these to deform the mesh.
//
// If the entity also has an AnimationComponent, the morph playback time is
// automatically synchronised to AnimationComponent::primaryTime.
// If not, the morph advances on its own using MorphComponent::time.
// ---------------------------------------------------------------------------
struct MorphComponent
{
    uint32_t primaryMorphClip = kInvalidAnimHandle; // -> MorphClipLibrary

    float time   = 0.f;   // independent playback time (used only when no AnimationComponent)
    bool  paused = false;
    bool  looping = true;
    uint8_t _pad[2]{};

    // Live output — written by AnimationSystem each frame
    static constexpr uint32_t MAX = 128;
    uint32_t count      = 0;           // number of active morph channels
    float    weights[MAX]{};           // per-target weight in [0, 1]
};

// ---------------------------------------------------------------------------
// SocketComponent — named attachment points on this character's skeleton.
//
// Combines "what sockets does this entity publish" (definition) with the
// per-frame world transforms that SocketSystem writes (cache). Followers
// reference sockets by index via FollowSocketComponent and read the cache
// directly — no pose-buffer round-trip needed.
//
// The struct is POD + fixed-size so it stays cache-friendly in a ComponentPool.
// Per-entity (not per-asset): two characters sharing a skeleton can publish
// different socket sets, and a prefab can override or extend a base list.
//
// Socket::localOffset is applied in BONE-local space (before the bone's
// current world matrix), matching the editor gizmo and original SocketSystem
// convention.
// ---------------------------------------------------------------------------
struct SocketComponent
{
    static constexpr uint32_t MAX = 16;

    struct Socket
    {
        char                name[64]    = {};         // user-facing id ("hand_r", "fx_root", …)
        uint32_t            boneIndex   = 0;          // index into SkeletonAsset::boneNames / bindPose
        DirectX::XMFLOAT4X4 localOffset;              // bone-local offset (static) — runtime source of truth
        DirectX::XMFLOAT4X4 worldTransform;           // written every frame by SocketSystem
        // Editor authoring representation for rotation. Translation is read
        // directly from localOffset._41/_42/_43; this field exists so the
        // inspector doesn't have to decompose quaternion→euler every frame
        // (that would jitter near gimbal lock). On serialize load the
        // matrix is decomposed once to seed this field.
        DirectX::XMFLOAT3   rotationEulerDeg = { 0.f, 0.f, 0.f };

        Socket()
        {
            DirectX::XMStoreFloat4x4(&localOffset,    DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixIdentity());
        }
    };

    Socket   sockets[MAX];
    uint32_t count = 0;

    bool Add(const char* socketName, uint32_t boneIdx)
    {
        if (count >= MAX) return false;
        Socket& s = sockets[count++];
        s.boneIndex = boneIdx;
        if (socketName)
        {
            const size_t len = std::strlen(socketName);
            const size_t copy = (len < sizeof(s.name) - 1) ? len : (sizeof(s.name) - 1);
            std::memcpy(s.name, socketName, copy);
            s.name[copy] = '\0';
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// PendingAnimBind — deferred animation binding for async-loaded clips.
// Attached to an entity by PrefabSerializer/SceneSerializer when the clip
// hasn't finished loading yet. AnimationClipSystem::Tick() polls this each
// frame and binds the clip once ready, then removes the component.
// ---------------------------------------------------------------------------
struct PendingAnimBind
{
    std::string animPath;
    uint32_t    handlePacked = 0; // AnimHandle::packed (avoids Resource:: dependency in ECS header)
};
