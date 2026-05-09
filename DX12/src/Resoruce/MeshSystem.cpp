#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Resource/MeshSystem.h"
#include "ECS/Components.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"
#include <cstring>

namespace Resource
{
    // -------------------------------------------------------------------------
    // Slot pool helpers  (caller must hold m_mutex)
    // -------------------------------------------------------------------------

    uint32_t MeshSystem::AllocSlot()
    {
        if (!m_freeList.empty())
        {
            uint32_t idx = m_freeList.back();
            m_freeList.pop_back();
            return idx;
        }
        m_slots.emplace_back();
        return static_cast<uint32_t>(m_slots.size() - 1);
    }

    void MeshSystem::FreeSlot(uint32_t index)
    {
        MeshEntry& e = m_slots[index];
        // Bump generation — never wraps to 0 (0 = null sentinel).
        e.generation  = (e.generation % Handle::MAX_GEN) + 1;
        e.alive       = false;
        e.vertexCount = 0;
        e.indexCount  = 0;
        m_freeList.push_back(index);
    }

    bool MeshSystem::IsValidHandle(Handle h) const
    {
        if (!h.IsValid()) return false;
        const uint32_t idx = h.Index();
        if (idx >= m_slots.size()) return false;
        const MeshEntry& e = m_slots[idx];
        return e.alive && e.generation == h.Generation();
    }

    // -------------------------------------------------------------------------
    // Acquire: upload MeshComponent CPU data to DEFAULT-heap GPU buffers.
    //
    // Vertex layout (32 bytes, interleaved):
    //   float3 position  (12 B)
    //   float3 normal    (12 B)
    //   float2 uv0       ( 8 B)
    // -------------------------------------------------------------------------

    MeshHandle MeshSystem::Acquire(const MeshComponent& mesh, IGraphicsDevice& gfx)
    {
        if (mesh.vertex_positions.empty() || mesh.indices.empty())
        {
            LOG_WARNING("MeshSystem::Acquire: empty vertex_positions or indices — skipping");
            return Handle{};
        }

        struct Vertex
        {
            float px, py, pz;
            float nx, ny, nz;
            float u, v;
        };

        const size_t vertexCount = mesh.vertex_positions.size();
        const size_t indexCount  = mesh.indices.size();

        std::vector<Vertex> vertices(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            vertices[i].px = mesh.vertex_positions[i].x;
            vertices[i].py = mesh.vertex_positions[i].y;
            vertices[i].pz = mesh.vertex_positions[i].z;

            if (i < mesh.vertex_normals.size())
            {
                vertices[i].nx = mesh.vertex_normals[i].x;
                vertices[i].ny = mesh.vertex_normals[i].y;
                vertices[i].nz = mesh.vertex_normals[i].z;
            }
            else
            {
                vertices[i].nx = 0.f;
                vertices[i].ny = 0.f;
                vertices[i].nz = 1.f;
            }

            if (i < mesh.vertex_uvset_0.size())
            {
                vertices[i].u = mesh.vertex_uvset_0[i].x;
                vertices[i].v = mesh.vertex_uvset_0[i].y;
            }
            else
            {
                vertices[i].u = 0.f;
                vertices[i].v = 0.f;
            }
        }

        // Vertex buffer — DEFAULT heap, RAW so MeshDescriptorHeap::RegisterRawBuffer can
        // create a ByteAddressBuffer SRV for PVF fetch (interleaved, 32 bytes/vertex).
        RHI::GPUBufferDesc vbDesc;
        vbDesc.size       = vertexCount * sizeof(Vertex);
        vbDesc.stride     = 0;
        vbDesc.usage      = RHI::Usage::DEFAULT;
        vbDesc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        vbDesc.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;

        // Index buffer — DEFAULT heap.
        RHI::GPUBufferDesc ibDesc;
        ibDesc.size       = indexCount * sizeof(uint32_t);
        ibDesc.stride     = sizeof(uint32_t);
        ibDesc.usage      = RHI::Usage::DEFAULT;
        ibDesc.bind_flags = RHI::BindFlag::INDEX_BUFFER | RHI::BindFlag::SHADER_RESOURCE;
        ibDesc.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;

        MeshEntry entry;
        entry.vertexCount = static_cast<uint32_t>(vertexCount);
        entry.indexCount  = static_cast<uint32_t>(indexCount);

        if (!gfx.CreateBuffer(vbDesc, entry.vertexBuffer, vertices.data()))
        {
            LOG_ERROR("MeshSystem: vertex buffer creation failed (%zu vertices)", vertexCount);
            return Handle{};
        }

        if (!gfx.CreateBuffer(ibDesc, entry.indexBuffer, mesh.indices.data()))
        {
            LOG_ERROR("MeshSystem: index buffer creation failed (%zu indices)", indexCount);
            gfx.DestroyBuffer(entry.vertexBuffer);
            return Handle{};
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const uint32_t idx = AllocSlot();
        MeshEntry& slot = m_slots[idx];
        if (slot.generation == 0) slot.generation = 1;  // never issue gen=0

        slot.vertexBuffer = std::move(entry.vertexBuffer);
        slot.indexBuffer  = std::move(entry.indexBuffer);
        slot.vertexCount  = entry.vertexCount;
        slot.indexCount   = entry.indexCount;
        slot.alive        = true;

        return Handle::Make(idx, ResourceType::Mesh, slot.generation);
    }

    // -------------------------------------------------------------------------
    // AcquireFromBlob — zero-copy fast path for bulk scene load. The caller
    // passes the already-interleaved vertex buffer + index buffer straight
    // from the .imsh payload (or from an ImshPack archive slice). Bypasses
    // the per-vertex de-interleave/re-interleave that Acquire(MeshComponent)
    // has to do when callers use MeshComponent's split-array layout.
    // -------------------------------------------------------------------------
    MeshHandle MeshSystem::AcquireFromBlob(const void*      vertexData,
                                           uint32_t         vertexCount,
                                           uint32_t         vertexStride,
                                           const uint32_t*  indexData,
                                           uint32_t         indexCount,
                                           IGraphicsDevice& gfx)
    {
        if (!vertexData || !indexData || vertexCount == 0 || indexCount == 0)
        {
            LOG_WARNING("MeshSystem::AcquireFromBlob: empty input");
            return Handle{};
        }

        RHI::GPUBufferDesc vbDesc;
        vbDesc.size       = size_t(vertexCount) * vertexStride;
        vbDesc.stride     = 0;
        vbDesc.usage      = RHI::Usage::DEFAULT;
        vbDesc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        vbDesc.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;

        RHI::GPUBufferDesc ibDesc;
        ibDesc.size       = size_t(indexCount) * sizeof(uint32_t);
        ibDesc.stride     = sizeof(uint32_t);
        ibDesc.usage      = RHI::Usage::DEFAULT;
        ibDesc.bind_flags = RHI::BindFlag::INDEX_BUFFER | RHI::BindFlag::SHADER_RESOURCE;
        ibDesc.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;

        MeshEntry entry;
        entry.vertexCount = vertexCount;
        entry.indexCount  = indexCount;

        if (!gfx.CreateBuffer(vbDesc, entry.vertexBuffer, vertexData))
        {
            LOG_ERROR("MeshSystem::AcquireFromBlob: VB create failed");
            return Handle{};
        }
        if (!gfx.CreateBuffer(ibDesc, entry.indexBuffer, indexData))
        {
            LOG_ERROR("MeshSystem::AcquireFromBlob: IB create failed");
            gfx.DestroyBuffer(entry.vertexBuffer);
            return Handle{};
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const uint32_t idx = AllocSlot();
        MeshEntry& slot = m_slots[idx];
        if (slot.generation == 0) slot.generation = 1;

        slot.vertexBuffer = std::move(entry.vertexBuffer);
        slot.indexBuffer  = std::move(entry.indexBuffer);
        slot.vertexCount  = entry.vertexCount;
        slot.indexCount   = entry.indexCount;
        slot.alive        = true;
        return Handle::Make(idx, ResourceType::Mesh, slot.generation);
    }

    // -------------------------------------------------------------------------
    // Release
    // -------------------------------------------------------------------------

    void MeshSystem::Release(MeshHandle handle, IGraphicsDevice& gfx)
    {
        if (!handle.IsValid())
            return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle))
            return;

        const uint32_t idx = handle.Index();
        gfx.DestroyBuffer(m_slots[idx].vertexBuffer);
        gfx.DestroyBuffer(m_slots[idx].indexBuffer);
        FreeSlot(idx);
    }

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    bool MeshSystem::IsValid(MeshHandle handle) const
    {
        if (!handle.IsValid()) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        return IsValidHandle(handle);
    }

    uint32_t MeshSystem::GetIndexCount(MeshHandle handle) const
    {
        if (!handle.IsValid()) return 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle)) return 0;
        return m_slots[handle.Index()].indexCount;
    }

    uint32_t MeshSystem::GetVertexCount(MeshHandle handle) const
    {
        if (!handle.IsValid()) return 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle)) return 0;
        return m_slots[handle.Index()].vertexCount;
    }

    const RHI::GPUBuffer* MeshSystem::GetVertexBuffer(MeshHandle handle) const
    {
        if (!handle.IsValid()) return nullptr;
        // No lock: main-thread only access.
        if (!IsValidHandle(handle)) return nullptr;
        return &m_slots[handle.Index()].vertexBuffer;
    }

    const RHI::GPUBuffer* MeshSystem::GetIndexBuffer(MeshHandle handle) const
    {
        if (!handle.IsValid()) return nullptr;
        // No lock: main-thread only access.
        if (!IsValidHandle(handle)) return nullptr;
        return &m_slots[handle.Index()].indexBuffer;
    }

    // -------------------------------------------------------------------------
    // Shutdown
    // -------------------------------------------------------------------------

    void MeshSystem::Shutdown(IGraphicsDevice& gfx)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_slots)
        {
            if (!entry.alive) continue;
            gfx.DestroyBuffer(entry.vertexBuffer);
            gfx.DestroyBuffer(entry.indexBuffer);
        }
        m_slots.clear();
        m_freeList.clear();
    }

} // namespace Resource
