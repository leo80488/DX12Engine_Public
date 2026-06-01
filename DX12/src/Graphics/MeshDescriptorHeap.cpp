#include "Graphics/MeshDescriptorHeap.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"
#include <cassert>
#include <cstring>
MeshDescriptorHeap::~MeshDescriptorHeap()
{
    if (!m_gfx) return;
    for (uint32_t i = 0; i < kFrameCount; ++i)
        if (m_meshDescMapped[i])
            m_gfx->UnmapBuffer(m_meshDescBuffers[i]);
}

void MeshDescriptorHeap::Init(IGraphicsDevice& gfx)
{
    auto& dx12  = static_cast<GraphicsDX12&>(gfx);
    m_gfx       = &dx12;
    m_device    = dx12.GetDevice();
    m_allocator = &dx12.GetCbvSrvUavAllocator();
    m_descSize  = m_allocator->GetDescriptorSize();

    // Allocate kMaxBuffers contiguous SRV slots for the bindless g_Buffers[] table.
    m_bufferRange = m_allocator->Allocate(kMaxBuffers);

    if (!m_bufferRange.IsValid())
    {
        LOG_ERROR("MeshDescriptorHeap: failed to allocate bindless SRV range");
        assert(m_bufferRange.IsValid() && "Descriptor Heap Too Small!");
        return;
    }

    // Triple-buffered UPLOAD-heap StructuredBuffer<MeshDescriptor>: one buffer
    // per frame in flight so the CPU's per-frame skinned-mesh `UpdateMeshThisFrame`
    // patches don't clobber a slot the GPU is still reading for a previous frame.
    RHI::GPUBufferDesc desc;
    desc.size       = static_cast<uint64_t>(kMaxMeshes) * sizeof(RHI::MeshDescriptor);
    desc.stride     = sizeof(RHI::MeshDescriptor);
    desc.usage      = RHI::Usage::UPLOAD;
    desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (!dx12.CreateBuffer(desc, m_meshDescBuffers[i]))
        {
            LOG_ERROR("MeshDescriptorHeap: failed to create MeshDescriptor buffer[%u]", i);
            return;
        }
        m_meshDescMapped[i] = dx12.MapBuffer(m_meshDescBuffers[i]);
        if (!m_meshDescMapped[i])
        {
            LOG_ERROR("MeshDescriptorHeap: failed to map MeshDescriptor buffer[%u]", i);
            return;
        }
    }

    // CPU-side master mirror; pre-reserve so RegisterMesh writes stay O(1).
    m_master.resize(kMaxMeshes);
    m_currentFrameSlot = 0;

    // Every ring slot starts unsynced — the first BeginFrame for each does a
    // full master->slot memcpy (covering at least the persistent prefix),
    // after which the incremental dirty-list path takes over.
    for (uint32_t i = 0; i < kFrameCount; ++i)
        m_slotNeedsFullResync[i] = true;

    // Create parallel AABB buffer (same slot count as MeshDescriptor).
    RHI::GPUBufferDesc aabbDesc;
    aabbDesc.size       = static_cast<uint64_t>(kMaxMeshes) * sizeof(MeshAABB);
    aabbDesc.stride     = sizeof(MeshAABB);
    aabbDesc.usage      = RHI::Usage::UPLOAD;
    aabbDesc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    if (dx12.CreateBuffer(aabbDesc, m_meshAABBBuffer))
        m_meshAABBMapped = dx12.MapBuffer(m_meshAABBBuffer);

    LOG_SUCCESS("MeshDescriptorHeap: initialised (%u buffer slots, %u mesh slots × %u frames)",
                kMaxBuffers, kMaxMeshes, kFrameCount);
}

void MeshDescriptorHeap::BeginFrame(uint32_t frameIndex)
{
    m_currentFrameSlot = frameIndex % kFrameCount;
    const uint32_t slot = m_currentFrameSlot;
    if (!m_meshDescMapped[slot]) return;

    auto* dst = static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[slot]);

    // Refresh the new frame's slot from the CPU master so stable descriptors
    // (RegisterMesh + UpdateMesh) propagate, and any TRANSIENT patches written
    // to this slot kFrameCount frames ago are wiped (callers must re-issue).
    //
    // Full path: taken on the slot's first visit after Init/OnWorldClear (it
    // was never synced to the current world) and as an overflow fallback when
    // the dirty list is as large as a full copy would be — so the incremental
    // path is never slower than the original unconditional memcpy. m_meshCount
    // starts at kPermanentMeshSlots, so the memcpy always covers the persistent
    // prefix (how RegisterPersistentMesh indices stay valid across reloads).
    if (m_slotNeedsFullResync[slot] || m_slotDirty[slot].size() >= m_meshCount)
    {
        std::memcpy(dst, m_master.data(),
                    static_cast<size_t>(m_meshCount) * sizeof(RHI::MeshDescriptor));
        m_slotNeedsFullResync[slot] = false;
    }
    else
    {
        // Incremental: recopy only the diverged indices (stable writes this
        // slot missed while inactive + transient patches it took last cycle).
        for (uint32_t idx : m_slotDirty[slot])
            if (idx < m_meshCount)          // guard: OnWorldClear may shrink count
                dst[idx] = m_master[idx];
    }
    m_slotDirty[slot].clear();
}

void MeshDescriptorHeap::MarkStableDirty(uint32_t idx)
{
    // A stable write patched m_master + the active slot directly, so only the
    // OTHER ring slots are now stale at idx and must recopy it next BeginFrame.
    // Slots already pending a full resync are skipped — their full memcpy will
    // pick up idx anyway, and this keeps dirty lists from ballooning during a
    // world load (when all three slots full-resync on first visit).
    for (uint32_t s = 0; s < kFrameCount; ++s)
        if (s != m_currentFrameSlot && !m_slotNeedsFullResync[s])
            m_slotDirty[s].push_back(idx);
}

// Internal: write the ByteAddressBuffer SRV for `resource` at the given
// bindless slot index. Shared by the world-scoped and persistent paths.
void MeshDescriptorHeap::WriteRawBufferSrv(uint32_t idx, ID3D12Resource* resource, UINT64 sizeBytes)
{
    // Compute per-slot handles by offsetting from the allocation base.
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot = m_bufferRange.GetCpuHandle();
    cpuSlot.ptr += static_cast<SIZE_T>(idx) * m_descSize;

    D3D12_CPU_DESCRIPTOR_HANDLE gpuCpuSlot = m_bufferRange.GetGpuCpuHandle();
    gpuCpuSlot.ptr += static_cast<SIZE_T>(idx) * m_descSize;

    // Create ByteAddressBuffer SRV at the CPU staging slot.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = DXGI_FORMAT_R32_TYPELESS;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Buffer.NumElements      = static_cast<UINT>(sizeBytes / 4);
    srvd.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
    m_device->CreateShaderResourceView(resource, &srvd, cpuSlot);

    // Copy to the shader-visible GPU heap slot.
    m_device->CopyDescriptorsSimple(1, gpuCpuSlot, cpuSlot,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint32_t MeshDescriptorHeap::RegisterRawBuffer(ID3D12Resource* resource, UINT64 sizeBytes)
{
    if (!resource || !m_device || !m_bufferRange.IsValid())
        return RHI::kInvalidBufferIndex;

    // Dedup: multiple meshes sharing the same VB/IB (e.g. the merged-per-
    // scene .imsh where N submeshes all reference one pair of buffers) must
    // resolve to the SAME bindless slot. Without this, the finite slot pool
    // (kMaxBuffers = 16384) runs dry after a few thousand RegisterSceneMesh
    // calls and every subsequent mesh silently fails to render.
    auto it = m_resourceToSlot.find(resource);
    if (it != m_resourceToSlot.end())
        return it->second;

    if (m_bufferCount >= kMaxBuffers)
    {
        LOG_ERROR("MeshDescriptorHeap: bindless buffer table full");
        return RHI::kInvalidBufferIndex;
    }

    const uint32_t idx = m_bufferCount++;
    m_resourceToSlot.emplace(resource, idx);
    WriteRawBufferSrv(idx, resource, sizeBytes);
    return idx;
}

uint32_t MeshDescriptorHeap::RegisterPersistentRawBuffer(ID3D12Resource* resource, UINT64 sizeBytes)
{
    if (!resource || !m_device || !m_bufferRange.IsValid())
        return RHI::kInvalidBufferIndex;

    if (m_persistentBufferCount >= kPermanentBufferSlots)
    {
        LOG_ERROR("MeshDescriptorHeap: persistent bindless buffer range exhausted "
                  "(%u/%u). Bump kPermanentBufferSlots.",
                  m_persistentBufferCount, kPermanentBufferSlots);
        return RHI::kInvalidBufferIndex;
    }

    const uint32_t idx = m_persistentBufferCount++;
    // NOT inserted into m_resourceToSlot — persistent slots intentionally
    // bypass world-scoped dedup so OnWorldClear can wipe that map without
    // also wiping the persistent → slot mapping. Persistent callers are
    // Init code and are expected to register each resource exactly once.
    WriteRawBufferSrv(idx, resource, sizeBytes);
    return idx;
}

uint32_t MeshDescriptorHeap::RegisterPersistentBuffer(const RHI::GPUBuffer& buf)
{
    if (!buf.IsValid()) return RHI::kInvalidBufferIndex;
    auto* dx12 = static_cast<GraphicsDX12*>(m_gfx);
    if (!dx12) return RHI::kInvalidBufferIndex;
    ID3D12Resource* resource = dx12->GetBufferResource(buf);
    if (!resource) return RHI::kInvalidBufferIndex;
    return RegisterPersistentRawBuffer(resource, buf.desc.size);
}

uint32_t MeshDescriptorHeap::RegisterMesh(const RHI::MeshDescriptor& desc)
{
    if (m_meshCount >= kMaxMeshes)
    {
        LOG_ERROR("MeshDescriptorHeap: mesh descriptor buffer full");
        return RHI::kInvalidBufferIndex;
    }
    if (!m_meshDescMapped[m_currentFrameSlot])
        return RHI::kInvalidBufferIndex;

    const uint32_t idx = m_meshCount++;
    // Master gets the canonical value; the active GPU slot is patched in-place
    // so the current frame already sees the new mesh.  Other slots pick it up
    // via their next BeginFrame() (dirty-tracked below).
    m_master[idx] = desc;
    static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[idx] = desc;
    MarkStableDirty(idx);
    return idx;
}

uint32_t MeshDescriptorHeap::RegisterPersistentMesh(const RHI::MeshDescriptor& desc)
{
    if (m_persistentMeshCount >= kPermanentMeshSlots)
    {
        LOG_ERROR("MeshDescriptorHeap: persistent mesh slot range exhausted "
                  "(%u/%u). Bump kPermanentMeshSlots.",
                  m_persistentMeshCount, kPermanentMeshSlots);
        return RHI::kInvalidBufferIndex;
    }
    if (!m_meshDescMapped[m_currentFrameSlot])
        return RHI::kInvalidBufferIndex;

    const uint32_t idx = m_persistentMeshCount++;
    // Persistent slot lives in the low reserved range; the world allocator
    // starts at kPermanentMeshSlots so there's no overlap. Write to master
    // (so all future BeginFrame() refreshes propagate it) plus the active GPU
    // slot (so the current frame can already render it).
    m_master[idx] = desc;
    static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[idx] = desc;
    MarkStableDirty(idx);
    return idx;
}

void MeshDescriptorHeap::UpdateMesh(uint32_t slot, const RHI::MeshDescriptor& desc)
{
    if (slot >= m_meshCount) return;
    m_master[slot] = desc;
    if (m_meshDescMapped[m_currentFrameSlot])
        static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[slot] = desc;
    MarkStableDirty(slot);  // other ring slots must catch up to the new value
}

void MeshDescriptorHeap::UpdateMeshThisFrame(uint32_t slot, const RHI::MeshDescriptor& desc)
{
    if (slot >= m_meshCount) return;
    // NOTE: writes ONLY to the active GPU slot, NOT to m_master.  The patch
    // evaporates at next BeginFrame() — caller MUST re-issue every frame.
    if (m_meshDescMapped[m_currentFrameSlot])
    {
        static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[slot] = desc;
        // This slot now diverges from master at `slot`; its next BeginFrame
        // must recopy master[slot] to wipe this transient patch.
        m_slotDirty[m_currentFrameSlot].push_back(slot);
    }
}

uint32_t MeshDescriptorHeap::RegisterBuffer(const RHI::GPUBuffer& buf)
{
    if (!buf.IsValid()) return RHI::kInvalidBufferIndex;
    auto* dx12 = static_cast<GraphicsDX12*>(m_gfx);
    if (!dx12) return RHI::kInvalidBufferIndex;
    ID3D12Resource* resource = dx12->GetBufferResource(buf);
    if (!resource) return RHI::kInvalidBufferIndex;
    return RegisterRawBuffer(resource, buf.desc.size);
}

RHI::MeshDescriptor MeshDescriptorHeap::GetMesh(uint32_t slot) const
{
    if (slot >= m_meshCount) return {};
    // CPU master is the source of truth for "stable" reads.  Any TRANSIENT
    // patches from UpdateMeshThisFrame are intentionally NOT visible here
    // (they're only in the active GPU slot's mapped buffer).
    return m_master[slot];
}

void MeshDescriptorHeap::SetMeshAABB(uint32_t slot, const MeshAABB& aabb)
{
    if (!m_meshAABBMapped || slot >= kMaxMeshes) return;
    static_cast<MeshAABB*>(m_meshAABBMapped)[slot] = aabb;
}

uint64_t MeshDescriptorHeap::GetBufferTableGpuHandle() const
{
    return m_bufferRange.GetGpuHandle().ptr;
}

void MeshDescriptorHeap::OnWorldClear()
{
    // World-scoped allocators rewind to the persistent base; persistent
    // counters (and persistent SRV descriptors at slot < kPermanent*Slots)
    // are intentionally NOT touched, so RegisterPersistent* slot indices
    // stay valid across the reload.
    //
    // m_resourceToSlot dedup map is world-scoped only (RegisterPersistent*
    // bypasses it), so a full clear is correct here.
    //
    // The CPU master vector keeps its memory; [0..kPermanentMeshSlots)
    // retains persistent descriptors, [kPermanentMeshSlots..oldMax) still
    // holds the previous world's bytes but nothing references them — new
    // RegisterMesh calls overwrite slot-by-slot as the new world loads, and
    // m_skinnedMeshDescCache / m_meshLibDescCache / per-entity meshDescriptorIdx
    // were all cleared upstream so no stale draw packet can address them.
    m_meshCount   = kPermanentMeshSlots;
    m_bufferCount = kPermanentBufferSlots;
    m_resourceToSlot.clear();

    // Force every ring slot to fully re-sync to the new world on its next
    // BeginFrame, and drop pending incremental dirty indices — they refer to
    // the old world's layout and m_meshCount just shrank.
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        m_slotNeedsFullResync[i] = true;
        m_slotDirty[i].clear();
    }
}
