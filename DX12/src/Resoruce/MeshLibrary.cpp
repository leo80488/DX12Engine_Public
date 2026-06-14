#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/MeshLibrary.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>
#include <fstream>

namespace Resource
{
    // -------------------------------------------------------------------------
    // Slot pool helpers  (caller must hold m_mutex)
    // -------------------------------------------------------------------------

    uint32_t MeshLibrary::AllocSlot()
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

    void MeshLibrary::FreeSlot(uint32_t index)
    {
        Slot& s = m_slots[index];
        if (s.pathHash != 0)
            m_pathHashToSlot.erase(s.pathHash);
        s.generation = (s.generation % Handle::MAX_GEN) + 1;  // never wraps to 0
        s.alive      = false;
        s.refCount   = 0;
        s.pathHash   = 0;
        s.sourcePath.clear();
        s.entries.clear();
        m_freeList.push_back(index);
    }

    uint64_t MeshLibrary::HashPath(const std::string& path)
    {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : path) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }

    bool MeshLibrary::IsValidHandle(Handle h) const
    {
        if (!h.IsValid()) return false;
        const uint32_t idx = h.Index();
        if (idx >= m_slots.size()) return false;
        const Slot& s = m_slots[idx];
        return s.alive && s.generation == h.Generation();
    }

    const MeshLibrary::Slot* MeshLibrary::GetSlot(Handle h) const
    {
        if (!IsValidHandle(h)) return nullptr;
        return &m_slots[h.Index()];
    }

    // -------------------------------------------------------------------------
    // Load — parse .meshlib blob, upload VB/IB, populate entry table.
    //
    // On-disk layout (see AssetHeader.h::MeshLibraryMetadata):
    //   [AssetHeader 24 B]
    //   [MeshLibraryMetadata 24 B]
    //   [MeshLibraryEntry × meshCount]  — 48 B each
    //   [vertices: vertexCount × vertexStride]
    //   [indices:  indexCount × 4]
    // -------------------------------------------------------------------------
    Handle MeshLibrary::Load(const std::string& path, IGraphicsDevice& gfx)
    {
        // Path-dedup fast path: same path already loaded → bump refcount and
        // return the existing handle. Eliminates GPU re-upload on same-world
        // reload (Bistro: ~10 .meshlib files, each 100 MB+ of VB+IB).
        const uint64_t pathHash = HashPath(path);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_pathHashToSlot.find(pathHash);
            if (it != m_pathHashToSlot.end() && it->second < m_slots.size())
            {
                Slot& s = m_slots[it->second];
                if (s.alive)
                {
                    ++s.refCount;
                    return Handle::Make(it->second,
                                        ResourceType::MeshLibrary,
                                        static_cast<uint16_t>(s.generation));
                }
            }
        }

        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(path, blob))
        {
            LOG_ERROR("MeshLibrary::Load: cannot open '%s'", path.c_str());
            return Handle{};
        }
        const std::streamoff size = static_cast<std::streamoff>(blob.size());
        if (size <= static_cast<std::streamoff>(sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)))
        {
            LOG_ERROR("MeshLibrary::Load: '%s' too small (%lld B)", path.c_str(),
                      static_cast<long long>(size));
            return Handle{};
        }
        if (!ValidateHeader(blob.data(), blob.size(), MAGIC_MESHLIB))
        {
            LOG_ERROR("MeshLibrary::Load: bad magic/version in '%s'", path.c_str());
            return Handle{};
        }

        const MeshLibraryMetadata* meta = GetMetadata<MeshLibraryMetadata>(blob.data());
        const uint8_t*             payload = GetPayload(blob.data());

        const uint32_t meshCount   = meta->meshCount;
        const uint32_t vertexCount = meta->vertexCount;
        const uint32_t indexCount  = meta->indexCount;
        const uint32_t vStride     = meta->vertexStride;
        const uint32_t iStride     = meta->indexStride;

        if (meshCount == 0 || vertexCount == 0 || indexCount == 0)
        {
            LOG_ERROR("MeshLibrary::Load: '%s' empty (meshes=%u verts=%u idx=%u)",
                      path.c_str(), meshCount, vertexCount, indexCount);
            return Handle{};
        }
        // Vertex layout is fully described by meta->flags via the shared
        // ComputeMeshLibVertexLayout() helper (pos+nrm+uv0 base 32B, plus
        // optional tangent/uv1/color appended in that order). The on-disk
        // stride MUST equal the flag-derived stride; a mismatch means a
        // corrupted or mid-version asset and we refuse to load. This accepts
        // legacy 32B (flags 0) and 48B (HAS_TANGENT) exactly as before, plus
        // the new uv1/color variants.
        const bool flagHasTangent = (meta->flags & MESHLIB_FLAG_HAS_TANGENT) != 0u;
        const uint32_t expectedStride =
            ComputeMeshLibVertexLayout(meta->flags).stride;
        if (vStride != expectedStride)
        {
            LOG_ERROR("MeshLibrary::Load: '%s' inconsistent layout — vertexStride=%u "
                      "but flags=0x%X imply stride=%u",
                      path.c_str(), vStride, meta->flags, expectedStride);
            return Handle{};
        }
        if (iStride != 4)
        {
            LOG_ERROR("MeshLibrary::Load: '%s' indexStride=%u (only 4 supported)",
                      path.c_str(), iStride);
            return Handle{};
        }

        // Bounds-check the payload: entry table + vertex blob + index blob
        // must fit inside the loaded bytes. A truncated blob would otherwise
        // give us a dangling pointer into freed memory once `blob` goes out
        // of scope (the GPU copy happens before that, but defensive is cheap).
        const size_t entryBytes  = size_t(meshCount) * sizeof(MeshLibraryEntry);
        const size_t vertexBytes = size_t(vertexCount) * vStride;
        const size_t indexBytes  = size_t(indexCount)  * iStride;
        const uint8_t* blobEnd   = blob.data() + blob.size();
        if (payload + entryBytes + vertexBytes + indexBytes > blobEnd)
        {
            LOG_ERROR("MeshLibrary::Load: '%s' truncated (need %zu B, have %zu B past payload)",
                      path.c_str(),
                      entryBytes + vertexBytes + indexBytes,
                      static_cast<size_t>(blobEnd - payload));
            return Handle{};
        }

        const MeshLibraryEntry* entryTable =
            reinterpret_cast<const MeshLibraryEntry*>(payload);
        const uint8_t*          vertexData = payload + entryBytes;
        const uint32_t*         indexData  =
            reinterpret_cast<const uint32_t*>(vertexData + vertexBytes);

        // ---- Upload GPU buffers ------------------------------------------------
        RHI::GPUBufferDesc vbDesc;
        vbDesc.size       = vertexBytes;
        vbDesc.stride     = 0;
        vbDesc.usage      = RHI::Usage::DEFAULT;
        vbDesc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        vbDesc.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;

        RHI::GPUBufferDesc ibDesc;
        ibDesc.size       = indexBytes;
        ibDesc.stride     = sizeof(uint32_t);
        ibDesc.usage      = RHI::Usage::DEFAULT;
        ibDesc.bind_flags = RHI::BindFlag::INDEX_BUFFER | RHI::BindFlag::SHADER_RESOURCE;
        ibDesc.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;

        RHI::GPUBuffer vb, ib;
        if (!gfx.CreateBuffer(vbDesc, vb, vertexData))
        {
            LOG_ERROR("MeshLibrary::Load: VB upload failed (%s, %zu B)",
                      path.c_str(), vertexBytes);
            return Handle{};
        }
        if (!gfx.CreateBuffer(ibDesc, ib, indexData))
        {
            LOG_ERROR("MeshLibrary::Load: IB upload failed (%s, %zu B)",
                      path.c_str(), indexBytes);
            gfx.DestroyBuffer(vb);
            return Handle{};
        }

        // ---- Build runtime entry table ----------------------------------------
        std::vector<Entry> entries(meshCount);
        for (uint32_t i = 0; i < meshCount; ++i)
        {
            const MeshLibraryEntry& src = entryTable[i];
            Entry& dst = entries[i];
            dst.vertexStart        = src.vertexStart;
            dst.vertexCount        = src.vertexCount;
            dst.indexStart         = src.indexStart;
            dst.indexCount         = src.indexCount;
            dst.aabbMin            = { src.aabbMin[0], src.aabbMin[1], src.aabbMin[2] };
            dst.aabbMax            = { src.aabbMax[0], src.aabbMax[1], src.aabbMax[2] };
            dst.defaultMaterialIdx = src.defaultMaterialIdx;
            dst.flags              = src.flags;
        }

        // ---- Commit slot ------------------------------------------------------
        std::lock_guard<std::mutex> lock(m_mutex);

        // Race recheck: another thread may have loaded the same path while we
        // were doing file I/O outside the lock. If so, drop our buffers and
        // return the winner's handle (refcount++).
        if (auto it = m_pathHashToSlot.find(pathHash);
            it != m_pathHashToSlot.end() && it->second < m_slots.size())
        {
            Slot& winner = m_slots[it->second];
            if (winner.alive)
            {
                ++winner.refCount;
                gfx.DestroyBuffer(vb);
                gfx.DestroyBuffer(ib);
                return Handle::Make(it->second,
                                    ResourceType::MeshLibrary,
                                    static_cast<uint16_t>(winner.generation));
            }
        }

        const uint32_t idx = AllocSlot();
        Slot& slot = m_slots[idx];
        if (slot.generation == 0) slot.generation = 1;
        slot.entries          = std::move(entries);
        slot.vb               = std::move(vb);
        slot.ib               = std::move(ib);
        slot.sourcePath       = path;
        slot.vertexCountTotal = vertexCount;
        slot.indexCountTotal  = indexCount;
        slot.vertexStride     = vStride;
        slot.hasTangent       = flagHasTangent;
        slot.flags            = meta->flags;
        slot.alive            = true;
        slot.refCount         = 1;
        slot.pathHash         = pathHash;
        m_pathHashToSlot[pathHash] = idx;

        LOG_SUCCESS("MeshLibrary: loaded '%s' (%u meshes, %u verts, %u idx, %.1f MB)",
                    path.c_str(), meshCount, vertexCount, indexCount,
                    double(vertexBytes + indexBytes) / (1024.0 * 1024.0));

        return Handle::Make(idx, ResourceType::MeshLibrary, slot.generation);
    }

    // -------------------------------------------------------------------------
    // Release
    // -------------------------------------------------------------------------

    void MeshLibrary::Release(Handle libHandle, IGraphicsDevice& gfx)
    {
        if (!libHandle.IsValid()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(libHandle)) return;

        Slot& slot = m_slots[libHandle.Index()];
        if (slot.refCount == 0) return;            // defensive: imbalanced Release
        if (--slot.refCount > 0) return;            // still referenced elsewhere

        if (slot.vb.IsValid()) gfx.DestroyBuffer(slot.vb);
        if (slot.ib.IsValid()) gfx.DestroyBuffer(slot.ib);
        FreeSlot(libHandle.Index());
    }

    // -------------------------------------------------------------------------
    // Query accessors
    // -------------------------------------------------------------------------

    bool MeshLibrary::IsValid(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return IsValidHandle(libHandle);
    }

    uint32_t MeshLibrary::GetMeshCount(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? static_cast<uint32_t>(s->entries.size()) : 0u;
    }

    const MeshLibrary::Entry* MeshLibrary::GetEntry(Handle libHandle, uint32_t meshId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        if (!s || meshId >= s->entries.size()) return nullptr;
        return &s->entries[meshId];
    }

    const RHI::GPUBuffer* MeshLibrary::GetVertexBuffer(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return (s && s->vb.IsValid()) ? &s->vb : nullptr;
    }

    const RHI::GPUBuffer* MeshLibrary::GetIndexBuffer(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return (s && s->ib.IsValid()) ? &s->ib : nullptr;
    }

    uint32_t MeshLibrary::GetVertexStride(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->vertexStride : 0u;
    }

    bool MeshLibrary::GetHasTangent(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->hasTangent : false;
    }

    uint32_t MeshLibrary::GetMeshLibFlags(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->flags : 0u;
    }

    // -------------------------------------------------------------------------
    // Editor enumeration helpers
    // -------------------------------------------------------------------------

    std::vector<Handle> MeshLibrary::GetLoadedHandles() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Handle> out;
        out.reserve(m_slots.size());
        for (uint32_t i = 0; i < m_slots.size(); ++i)
        {
            const Slot& s = m_slots[i];
            if (!s.alive) continue;
            out.push_back(Handle::Make(i, ResourceType::MeshLibrary, s.generation));
        }
        return out;
    }

    MeshLibrary::TotalsView MeshLibrary::GetTotals(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        if (!s) return {};
        return { s->vertexCountTotal, s->indexCountTotal };
    }

    std::string MeshLibrary::GetSourcePath(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->sourcePath : std::string{};
    }

    // -------------------------------------------------------------------------
    // Shutdown
    // -------------------------------------------------------------------------

    void MeshLibrary::Shutdown(IGraphicsDevice& gfx)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& slot : m_slots)
        {
            if (!slot.alive) continue;
            if (slot.vb.IsValid()) gfx.DestroyBuffer(slot.vb);
            if (slot.ib.IsValid()) gfx.DestroyBuffer(slot.ib);
            slot.alive = false;
        }
        m_slots.clear();
        m_freeList.clear();
    }

} // namespace Resource
