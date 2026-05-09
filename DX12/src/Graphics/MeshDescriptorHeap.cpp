#include "Graphics/MeshDescriptorHeap.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"
#include <cassert>
MeshDescriptorHeap::~MeshDescriptorHeap()
{
    if (m_meshDescMapped && m_gfx)
        m_gfx->UnmapBuffer(m_meshDescBuffer);
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

    // Create UPLOAD-heap StructuredBuffer<MeshDescriptor> via IGraphicsDevice so
    // it lives in the buffer pool and can be bound via SetRootBufferSRV.
    RHI::GPUBufferDesc desc;
    desc.size       = static_cast<uint64_t>(kMaxMeshes) * sizeof(RHI::MeshDescriptor);
    desc.stride     = sizeof(RHI::MeshDescriptor);
    desc.usage      = RHI::Usage::UPLOAD;
    desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    if (!dx12.CreateBuffer(desc, m_meshDescBuffer))
    {
        LOG_ERROR("MeshDescriptorHeap: failed to create MeshDescriptor buffer");
        return;
    }
    m_meshDescMapped = dx12.MapBuffer(m_meshDescBuffer);

    // Create parallel AABB buffer (same slot count as MeshDescriptor).
    RHI::GPUBufferDesc aabbDesc;
    aabbDesc.size       = static_cast<uint64_t>(kMaxMeshes) * sizeof(MeshAABB);
    aabbDesc.stride     = sizeof(MeshAABB);
    aabbDesc.usage      = RHI::Usage::UPLOAD;
    aabbDesc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    if (dx12.CreateBuffer(aabbDesc, m_meshAABBBuffer))
        m_meshAABBMapped = dx12.MapBuffer(m_meshAABBBuffer);

    LOG_SUCCESS("MeshDescriptorHeap: initialised (%u buffer slots, %u mesh slots)",
                kMaxBuffers, kMaxMeshes);
}

uint32_t MeshDescriptorHeap::RegisterRawBuffer(ID3D12Resource* resource, UINT64 sizeBytes)
{
    if (!resource || !m_device || !m_bufferRange.IsValid())
        return RHI::kInvalidBufferIndex;

    // Dedup: multiple meshes sharing the same VB/IB (e.g. the merged-per-
    // scene .imsh where N submeshes all reference one pair of buffers) must
    // resolve to the SAME bindless slot. Without this, the finite slot pool
    // (kMaxBuffers = 4096) runs dry after a few thousand RegisterSceneMesh
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
    return idx;
}

uint32_t MeshDescriptorHeap::RegisterMesh(const RHI::MeshDescriptor& desc)
{
    if (m_meshCount >= kMaxMeshes)
    {
        LOG_ERROR("MeshDescriptorHeap: mesh descriptor buffer full");
        return RHI::kInvalidBufferIndex;
    }
    if (!m_meshDescMapped)
        return RHI::kInvalidBufferIndex;

    const uint32_t idx = m_meshCount++;
    static_cast<RHI::MeshDescriptor*>(m_meshDescMapped)[idx] = desc;
    return idx;
}

void MeshDescriptorHeap::UpdateMesh(uint32_t slot, const RHI::MeshDescriptor& desc)
{
    if (!m_meshDescMapped || slot >= m_meshCount) return;
    static_cast<RHI::MeshDescriptor*>(m_meshDescMapped)[slot] = desc;
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
    if (!m_meshDescMapped || slot >= m_meshCount) return {};
    return static_cast<const RHI::MeshDescriptor*>(m_meshDescMapped)[slot];
}

void MeshDescriptorHeap::SetMeshAABB(uint32_t slot, const MeshAABB& aabb)
{
    if (!m_meshAABBMapped || slot >= kMaxMeshes) return;
    static_cast<MeshAABB*>(m_meshAABBMapped)[slot] = aabb;
}

D3D12_GPU_DESCRIPTOR_HANDLE MeshDescriptorHeap::GetBufferTableGpuHandle() const
{
    return m_bufferRange.GetGpuHandle();
}
