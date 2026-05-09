#pragma once

// MeshDescriptorHeap — manages two GPU-side PVF resources:
//
//  1. Bindless buffer table: a contiguous block of kMaxBuffers ByteAddressBuffer SRV
//     descriptors in the CBV/SRV/UAV heap.  Bound as the g_Buffers[] descriptor table
//     (root param kBindlessSlot) each draw.  RegisterRawBuffer() adds a ByteAddressBuffer
//     and returns a stable bindless index used in StreamDescriptor::bufferIndex.
//
//  2. MeshDescriptor StructuredBuffer: a persistently-mapped UPLOAD-heap GPUBuffer holding
//     RHI::MeshDescriptor entries.  Accessible as a RHI::GPUBuffer via GetMeshDescBuffer()
//     so callers can bind it via IGraphicsDevice::SetRootBufferSRV.
//     RegisterMesh() writes a descriptor and returns a stable slot index.
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

class GraphicsDX12;
class IGraphicsDevice;

class MeshDescriptorHeap
{
public:
    static constexpr uint32_t kMaxBuffers = 4096;   // bindless g_Buffers[] slots — must match kMaxBindlessBuffers in GraphicsDX12.cpp and g_Buffers[] in pvf_fetch.hlsli
    // MeshDescriptor slots — one per unique (mesh, submesh-range) pair.
    // Bistro-class scenes can have ~22 k submeshes in the merged .imsh, so
    // 4 k is too small. 65 536 × 96 B = 6 MB — trivial memory cost.
    static constexpr uint32_t kMaxMeshes  = 65536;

    MeshDescriptorHeap() = default;
    ~MeshDescriptorHeap();

    // Must be called once before any Register* calls.
    // Accepts IGraphicsDevice& for caller portability; downcasts to the DX12
    // backend internally (MeshDescriptorHeap is a DX12-specific helper).
    void Init(IGraphicsDevice& gfx);

    // Register a DEFAULT-heap ByteAddressBuffer resource as a bindless SRV.
    // Returns the bindless buffer index (== StreamDescriptor::bufferIndex).
    uint32_t RegisterRawBuffer(ID3D12Resource* resource, UINT64 sizeBytes);

    // Write a MeshDescriptor to the persistently-mapped GPU buffer.
    // Returns the stable mesh slot index (== DrawPacket::meshDescriptorIndex).
    uint32_t RegisterMesh(const RHI::MeshDescriptor& desc);

    // Overwrite an existing mesh descriptor slot in-place (for per-frame skinned mesh updates).
    // slot must be a value previously returned by RegisterMesh().
    void UpdateMesh(uint32_t slot, const RHI::MeshDescriptor& desc);

    // Read back the current value of a mesh descriptor slot (for skinned-mesh patching).
    // Returns a zeroed descriptor if slot is out of range.
    RHI::MeshDescriptor GetMesh(uint32_t slot) const;

    // Register a GPUBuffer as a ByteAddressBuffer SRV in the bindless table.
    // Convenience wrapper around RegisterRawBuffer that resolves the D3D12 resource internally.
    // Returns the bindless buffer index, or kInvalidBufferIndex on failure.
    uint32_t RegisterBuffer(const RHI::GPUBuffer& buf);

    // GPU handle of the start of the bindless SRV table (pass to kBindlessSlot).
    D3D12_GPU_DESCRIPTOR_HANDLE GetBufferTableGpuHandle() const;

    // The MeshDescriptor StructuredBuffer as a RHI::GPUBuffer handle.
    // Valid after Init(). Bind via IGraphicsDevice::SetRootBufferSRV at kMeshDescSlot.
    const RHI::GPUBuffer& GetMeshDescBuffer() const { return m_meshDescBuffer; }

    // Per-mesh local AABB buffer (parallel to MeshDescriptor buffer).
    // Each entry is 6 floats: {minX, minY, minZ, maxX, maxY, maxZ} = 24 bytes.
    struct MeshAABB { float minX, minY, minZ, maxX, maxY, maxZ; };
    void SetMeshAABB(uint32_t slot, const MeshAABB& aabb);
    const RHI::GPUBuffer& GetMeshAABBBuffer() const { return m_meshAABBBuffer; }

    uint32_t GetBufferCount() const { return m_bufferCount; }
    uint32_t GetMeshCount()   const { return m_meshCount;   }

private:
    IGraphicsDevice*         m_gfx        = nullptr;
    ID3D12Device*            m_device     = nullptr;
    DescriptorHeapAllocator* m_allocator  = nullptr;
    UINT                     m_descSize   = 0;

    // Contiguous SRV block for g_Buffers[]
    DescriptorAllocation     m_bufferRange;
    uint32_t                 m_bufferCount = 0;
    // Resource → slot dedup. The bindless table is a finite pool (kMaxBuffers),
    // so when N mesh entities share the same VB/IB (e.g. the merged-per-scene
    // .imsh layout where a whole Bistro shares one pair of buffers), they
    // must all resolve to the SAME slot instead of consuming a fresh one
    // per registration. Without this, 22 k submeshes × 2 = 44 k calls
    // overflow the 4 k-slot bindless table.
    std::unordered_map<ID3D12Resource*, uint32_t> m_resourceToSlot;

    // Persistently-mapped UPLOAD buffer for MeshDescriptor StructuredBuffer.
    // Created through IGraphicsDevice so it lives in m_bufferPool and can be
    // bound directly via SetRootBufferSRV.
    RHI::GPUBuffer   m_meshDescBuffer;
    void*            m_meshDescMapped = nullptr;
    uint32_t         m_meshCount      = 0;

    // Parallel AABB buffer (same slot indexing as MeshDescriptor).
    RHI::GPUBuffer   m_meshAABBBuffer;
    void*            m_meshAABBMapped = nullptr;
};
