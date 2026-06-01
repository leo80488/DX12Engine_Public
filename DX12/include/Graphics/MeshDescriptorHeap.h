#pragma once

// MeshDescriptorHeap — manages two GPU-side PVF resources:
//
//  1. Bindless buffer table: a contiguous block of kMaxBuffers ByteAddressBuffer SRV
//     descriptors in the CBV/SRV/UAV heap.  Bound as the g_Buffers[] descriptor table
//     (root param kBindlessSlot) each draw.  RegisterBuffer(const RHI::GPUBuffer&)
//     adds a ByteAddressBuffer and returns a stable bindless index used in
//     StreamDescriptor::bufferIndex.
//
//  2. MeshDescriptor StructuredBuffer: a persistently-mapped UPLOAD-heap GPUBuffer holding
//     RHI::MeshDescriptor entries.  Accessible as a RHI::GPUBuffer via GetMeshDescBuffer()
//     so callers can bind it via IGraphicsDevice::SetRootBufferSRV.
//     RegisterMesh() writes a descriptor and returns a stable slot index.
//
//  Slot lifetime: world-scoped (RegisterBuffer / RegisterMesh — cleared on
//  OnWorldClear) vs engine-lifetime (RegisterPersistentBuffer /
//  RegisterPersistentMesh — survive every world reload, suitable for caching
//  the returned slot index at Init time).
//
//  Class is a DX12-specific helper: its PUBLIC surface is pure RHI types
//  (RHI::GPUBuffer, RHI::MeshDescriptor, uint64_t handles), but its private
//  implementation still calls into D3D12 directly for descriptor-heap SRV
//  writes — RHI does not abstract bindless table construction.
//
//  Call Init() once at startup.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl.h>
#include "d3d12.h"

#include "Graphics/DescriptorHeapAllocator.h"
#include "Graphics/GraphicsStruct.h"

#include <unordered_map>
#include <vector>

class GraphicsDX12;
class IGraphicsDevice;

class MeshDescriptorHeap
{
public:
    // Bindless g_Buffers[] slots — must match kMaxBindlessBuffers in
    // GraphicsDX12.cpp and the explicit array sizes in pvf_fetch.hlsli
    // (`g_Buffers[N]`) + DDGIRayTrace.cs.hlsl (`g_DDGIBuffers[N]`).
    //
    // Sized for character-heavy AA: ~30 NPC × 25 submesh × 2 buffers
    // = ~1500 skinned slots + a few hundred static + 256 persistent
    // = ~2000 typical, leaving ~14k headroom. Bistro stress test uses
    // ~80 slots total. Hard cap is descriptor-heap tier (Tier 2/3 = 1M
    // shader-visible descriptors), so 16k is well under hardware limits.
    static constexpr uint32_t kMaxBuffers = 16384;
    // MeshDescriptor slots — one per unique (mesh, submesh-range) pair.
    // Bistro-class scenes carry ~22 k submeshes in the merged .imsh; city-
    // scale aggregates of several such scenes push past 100 k. Sized for
    // 512 k slots: 524 288 × 96 B = 48 MB per buffer × 3 frames = 144 MB
    // GPU upload, + 12 MB AABB. Heavy but bounded; leave headroom for
    // procedural geometry growth without another bump.
    static constexpr uint32_t kMaxMeshes  = 524288;
    // MUST match GraphicsDX12::FrameCount.  Triple-buffering the descriptor
    // ringblock prevents the CPU's per-frame skinned-mesh `UpdateMesh()` patches
    // (which mutate `position.bufferIndex`/`byteOffset`) from racing the still-
    // in-flight previous frame's GPU reads — without this, late passes such as
    // OutlinePass see the *next* frame's bindless index pointing at a not-yet-
    // written ring slice, producing visibly stale skinned geometry (the
    // "inverted-hull lags behind moving character" bug).
    static constexpr uint32_t kFrameCount = 3;

    // Slot range [0..kPermanent*Slots) is reserved for ENGINE-LIFETIME
    // registrations (procedural primitives, skinned-vertex ring, etc.).
    // OnWorldClear() does NOT reset these — slots stay valid across every
    // world reload, so callers can cache the returned index at Init time.
    //
    // Current consumers (~42 buffers / 6 meshes used):
    //   buffers: 5 primitives × ~6 streams + billboard × 6 streams
    //          + skinned vertex ring (3 frames × 2 streams = 6) = ~42
    //   meshes : 5 primitives + 1 billboard = 6
    //
    // Sized at 4–10× current usage so future engine-lifetime additions
    // (more primitives, prev-pos ring streams, debug meshes, etc.) don't
    // need a constant bump + recompile. Cost is purely the BeginFrame
    // memcpy covering an extra ~60 unused mesh slots × 96 B ≈ 5.6 KB
    // per frame — far below noise. Bindless table itself is kMaxBuffers
    // = 16384 so taking 256 persistent slots still leaves 16128 for world.
    static constexpr uint32_t kPermanentBufferSlots = 256;
    static constexpr uint32_t kPermanentMeshSlots   = 64;

    MeshDescriptorHeap() = default;
    ~MeshDescriptorHeap();

    // Must be called once before any Register* calls.
    // Accepts IGraphicsDevice& for caller portability; downcasts to the DX12
    // backend internally (MeshDescriptorHeap is a DX12-specific helper).
    void Init(IGraphicsDevice& gfx);

    // Per-frame: pick the GPU slot the frame just released, then refresh it
    // from the CPU-side master so any stable Register/UpdateMesh writes done
    // during previous frames are present.  Must be called once at the start
    // of the frame, BEFORE any per-frame UpdateMeshThisFrame() patches.
    void BeginFrame(uint32_t frameIndex);

    // Write a MeshDescriptor to the persistently-mapped GPU buffer.
    // Returns the stable mesh slot index (== DrawPacket::meshDescriptorIndex).
    // World-scoped: slot is invalidated by OnWorldClear().
    uint32_t RegisterMesh(const RHI::MeshDescriptor& desc);

    // PERSISTENT (engine-lifetime) registration. Allocated from the reserved
    // low slot range [0..kPermanent*Slots); the returned index stays valid
    // across every world reload so Init-time callers can stash it once.
    //
    // No resource-dedup is performed: callers are expected to invoke each
    // persistent register exactly once per app lifetime (Init code, not
    // hot paths). Each call consumes one slot until the cap is hit.
    uint32_t RegisterPersistentBuffer(const RHI::GPUBuffer& buf);
    uint32_t RegisterPersistentMesh  (const RHI::MeshDescriptor& desc);

    // STABLE update: persists across frames (master + current GPU slot).
    // Use for descriptors whose value is expected to survive multiple frames
    // (e.g. afterimage snapshot patches pointing at a non-ring pool buffer).
    void UpdateMesh(uint32_t slot, const RHI::MeshDescriptor& desc);

    // TRANSIENT update: written ONLY to the current frame's GPU slot, NOT to
    // the master.  Use for per-frame patches whose value depends on the
    // current frame slot (e.g. SkinnedMeshSubsystem's per-frame patch where
    // `position.bufferIndex` references the per-frame skinned-vertex ring).
    // Next BeginFrame() will reset that slot from master, so the caller MUST
    // re-issue this patch every frame the override is needed.
    void UpdateMeshThisFrame(uint32_t slot, const RHI::MeshDescriptor& desc);

    // Read back the current value of a mesh descriptor slot (for skinned-mesh patching).
    // Returns a zeroed descriptor if slot is out of range.
    RHI::MeshDescriptor GetMesh(uint32_t slot) const;

    // Register a GPUBuffer as a ByteAddressBuffer SRV in the bindless table.
    // World-scoped: slot is invalidated by OnWorldClear() — for engine-lifetime
    // buffers use RegisterPersistentBuffer instead.
    // Returns the bindless buffer index, or kInvalidBufferIndex on failure.
    uint32_t RegisterBuffer(const RHI::GPUBuffer& buf);

    // GPU handle of the start of the bindless SRV table (pass to kBindlessSlot).
    // Returned as a raw uint64_t (matches RHI convention used elsewhere for
    // GPU descriptor handles — e.g. IGraphicsDevice::GetTextureSRVGpuHandle).
    uint64_t GetBufferTableGpuHandle() const;

    // The MeshDescriptor StructuredBuffer as a RHI::GPUBuffer handle.
    // Returns the CURRENT FRAME'S buffer — caller must rebind every frame.
    const RHI::GPUBuffer& GetMeshDescBuffer() const { return m_meshDescBuffers[m_currentFrameSlot]; }

    // Per-mesh local AABB buffer (parallel to MeshDescriptor buffer).
    // Each entry is 6 floats: {minX, minY, minZ, maxX, maxY, maxZ} = 24 bytes.
    struct MeshAABB { float minX, minY, minZ, maxX, maxY, maxZ; };
    void SetMeshAABB(uint32_t slot, const MeshAABB& aabb);
    const RHI::GPUBuffer& GetMeshAABBBuffer() const { return m_meshAABBBuffer; }

    uint32_t GetBufferCount() const { return m_bufferCount; }
    uint32_t GetMeshCount()   const { return m_meshCount;   }

    // Reset WORLD-SCOPED slot allocators + dedup map back to the persistent
    // base. Required on world reload — without it, every reload re-registers
    // every mesh and buffer at higher slot indices and the bindless table
    // exhausts within ~3 reloads of a Bistro-scale scene.
    //
    // Slots in the persistent range [0..kPermanent*Slots) are UNTOUCHED, so
    // anything registered via RegisterPersistent* at Init keeps its slot
    // index and its bindless table SRV across the reload — no caller-side
    // re-registration needed. This was the fix for the "skinned mesh becomes
    // garbage / disappears after every world reload" bug where each owner
    // had to manually re-register its long-lived bindless buffers.
    //
    // GPU safety: caller MUST FlushAndWait before invoking; world slots
    // (their SRV descriptors at indices >= kPermanent*Slots) may still be
    // bound by an in-flight CL. EditorLayer's "Load World..." path flushes
    // before triggering OnWorldClear for this reason.
    //
    // The descriptor heap range stays allocated; only the world-scoped
    // logical contents are reset.
    void OnWorldClear();

private:
    // Internal D3D12-typed register paths — kept private so the public surface
    // stays purely RHI-typed. RegisterBuffer / RegisterPersistentBuffer (the
    // public RHI wrappers) resolve the ID3D12Resource* via GraphicsDX12 and
    // forward here.
    uint32_t RegisterRawBuffer          (ID3D12Resource* resource, UINT64 sizeBytes);
    uint32_t RegisterPersistentRawBuffer(ID3D12Resource* resource, UINT64 sizeBytes);

    // Internal: write the ByteAddressBuffer SRV for `resource` at the given
    // absolute bindless slot. Shared by world + persistent register paths.
    void WriteRawBufferSrv(uint32_t slot, ID3D12Resource* resource, UINT64 sizeBytes);

    IGraphicsDevice*         m_gfx        = nullptr;
    ID3D12Device*            m_device     = nullptr;
    DescriptorHeapAllocator* m_allocator  = nullptr;
    UINT                     m_descSize   = 0;

    // Contiguous SRV block for g_Buffers[]. Layout:
    //   [0 .. kPermanentBufferSlots)              persistent (Init-time)
    //   [kPermanentBufferSlots .. m_bufferCount)  world-scoped (this reload)
    //   [m_bufferCount .. kMaxBuffers)            unused
    DescriptorAllocation     m_bufferRange;
    uint32_t                 m_persistentBufferCount = 0;                    // grows on RegisterPersistent*; never reset
    uint32_t                 m_bufferCount           = kPermanentBufferSlots; // world allocator; OnWorldClear resets to base
    // Resource → slot dedup for WORLD-scoped registrations only. The bindless
    // table is a finite pool (kMaxBuffers), so when N mesh entities share the
    // same VB/IB (e.g. the merged-per-scene .imsh layout where a whole Bistro
    // shares one pair of buffers), they must all resolve to the SAME slot
    // instead of consuming a fresh one per registration. Without this, 22 k
    // submeshes × 2 = 44 k calls overflow the 4 k-slot table.
    // Persistent registrations bypass this map (Init code is expected to
    // register each resource at most once).
    std::unordered_map<ID3D12Resource*, uint32_t> m_resourceToSlot;

    // Persistently-mapped UPLOAD buffers for MeshDescriptor StructuredBuffer
    // — kFrameCount-deep ring (one buffer per frame in flight).  Bound buffer
    // rotates per frame via BeginFrame() to break the CPU-write/GPU-read race
    // on per-frame skinning patches.
    RHI::GPUBuffer   m_meshDescBuffers[kFrameCount];
    void*            m_meshDescMapped [kFrameCount]{};
    uint32_t         m_persistentMeshCount = 0;                  // grows on RegisterPersistentMesh; never reset
    uint32_t         m_meshCount           = kPermanentMeshSlots; // world allocator; OnWorldClear resets to base
    uint32_t         m_currentFrameSlot    = 0;

    // CPU-side master copy: the source of truth for "stable" descriptors
    // (RegisterMesh + stable UpdateMesh).  BeginFrame() refreshes the new
    // frame's GPU slot from this so stable writes propagate and the slot's
    // previous tenant's transient patches are wiped.
    std::vector<RHI::MeshDescriptor> m_master;

    // ---- Incremental BeginFrame refresh ------------------------------------
    // Avoid re-copying the whole [0..m_meshCount) master prefix into the
    // rotated GPU slot every frame (multi-MB on 22k-100k-submesh worlds, of
    // which almost all bytes are unchanged since this slot was last visited
    // kFrameCount frames ago). Each ring slot instead tracks the indices where
    // its contents diverge from m_master and recopies only those. A slot
    // diverges in exactly two ways, both fixed by recopying from master:
    //   (a) a STABLE write (RegisterMesh / RegisterPersistentMesh / UpdateMesh)
    //       changed m_master while this slot was inactive;
    //   (b) a TRANSIENT UpdateMeshThisFrame patched this slot directly last
    //       cycle and must be wiped back to the master value.
    // The list is a CONSERVATIVE superset — duplicate or already-matching
    // indices only cost a harmless idempotent recopy, never incorrectness.
    std::vector<uint32_t> m_slotDirty[kFrameCount];
    // Forces a full master->slot memcpy on this slot's next BeginFrame. Set at
    // Init and OnWorldClear (slot was never synced to the current world); also
    // the overflow fallback (see BeginFrame) so we never copy MORE than the
    // original full-prefix memcpy.
    bool m_slotNeedsFullResync[kFrameCount]{};

    // Record that master[idx] changed via a stable write: every ring slot
    // EXCEPT the just-written active one must recopy it on its next BeginFrame.
    void MarkStableDirty(uint32_t idx);

    // Parallel AABB buffer (same slot indexing as MeshDescriptor).
    RHI::GPUBuffer   m_meshAABBBuffer;
    void*            m_meshAABBMapped = nullptr;
};
