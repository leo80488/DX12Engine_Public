#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Resource/MaterialSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"
#include <cstring>
#include <algorithm>

namespace Resource
{
    // -------------------------------------------------------------------------
    // CBV alignment helper — D3D12 requires CBVs to be 256-byte aligned.
    // -------------------------------------------------------------------------

    static constexpr uint64_t Align256(uint64_t size)
    {
        return (size + 255ull) & ~255ull;
    }

    // -------------------------------------------------------------------------
    // Slot pool helpers  (caller must hold m_mutex)
    // -------------------------------------------------------------------------

    uint32_t MaterialSystem::AllocSlot()
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

    void MaterialSystem::FreeSlot(uint32_t index)
    {
        MaterialEntry& e = m_slots[index];
        // Bump generation — never wraps to 0 (0 = null sentinel).
        e.generation = (e.generation % Handle::MAX_GEN) + 1;
        e.alive      = false;
        m_freeList.push_back(index);
    }

    bool MaterialSystem::IsValidHandle(Handle h) const
    {
        if (!h.IsValid()) return false;
        const uint32_t idx = h.Index();
        if (idx >= m_slots.size()) return false;
        const MaterialEntry& e = m_slots[idx];
        return e.alive && e.generation == h.Generation();
    }

    // -------------------------------------------------------------------------
    // Create
    // -------------------------------------------------------------------------

    MaterialHandle MaterialSystem::Create()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint32_t idx = AllocSlot();
        MaterialEntry& slot = m_slots[idx];
        if (slot.generation == 0) slot.generation = 1;  // never issue gen=0
        slot.asset    = MaterialAsset{};
        slot.cbvValid = false;
        slot.alive    = true;
        return Handle::Make(idx, ResourceType::Material, slot.generation);
    }

    // -------------------------------------------------------------------------
    // Destroy
    // -------------------------------------------------------------------------

    void MaterialSystem::Destroy(MaterialHandle handle, TextureSystem& texSys, IGraphicsDevice& gfx)
    {
        if (!handle.IsValid())
            return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle))
            return;

        const uint32_t idx = handle.Index();
        MaterialEntry& entry = m_slots[idx];

        for (uint32_t i = 0; i < entry.asset.textureCount; ++i)
        {
            if (entry.asset.textureSlots[i].IsValid())
                texSys.Release(entry.asset.textureSlots[i], gfx);
        }

        if (entry.cbvValid)
            gfx.DestroyBuffer(entry.cbv);

        FreeSlot(idx);
    }

    // -------------------------------------------------------------------------
    // Get
    // -------------------------------------------------------------------------

    MaterialAsset* MaterialSystem::Get(MaterialHandle handle)
    {
        if (!handle.IsValid()) return nullptr;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle)) return nullptr;
        return &m_slots[handle.Index()].asset;
    }

    const MaterialAsset* MaterialSystem::Get(MaterialHandle handle) const
    {
        if (!handle.IsValid()) return nullptr;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle)) return nullptr;
        return &m_slots[handle.Index()].asset;
    }

    // -------------------------------------------------------------------------
    // ResolveTextures
    // -------------------------------------------------------------------------

    void MaterialSystem::ResolveTextures(MaterialHandle    handle,
                                         const std::string paths[],
                                         uint32_t          pathCount,
                                         TextureSystem&    texSys,
                                         ResourceManager&  rm,
                                         IGraphicsDevice&  gfx)
    {
        if (!handle.IsValid())
            return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle))
            return;

        MaterialAsset& asset = m_slots[handle.Index()].asset;
        const uint32_t count = (std::min)(pathCount, kMaxTextureSlots);

        for (uint32_t i = 0; i < count; ++i)
        {
            if (asset.textureSlots[i].IsValid())
                texSys.Release(asset.textureSlots[i], gfx);

            asset.textureSlots[i] = paths[i].empty()
                ? Handle{}
                : texSys.Acquire(paths[i], rm, gfx);
        }

        asset.textureCount = count;
        asset.dirty        = true;
    }

    // -------------------------------------------------------------------------
    // FlushToCBV — write MaterialGPUData into the UPLOAD-heap CBV.
    // Caller must hold m_mutex.
    // -------------------------------------------------------------------------

    void MaterialSystem::FlushToCBV(MaterialEntry& entry, const TextureSystem& texSys, IGraphicsDevice& gfx)
    {
        // M3-Day-1: MaterialGPUData no longer has a generic `params[i][N]` array;
        // engine PBR fields are explicitly named. The legacy MaterialAsset
        // generic-param path here predates the shift to MaterialComponent +
        // Renderer::WriteMatSlot and has no live consumers (GetCBV() is
        // unreferenced). We keep the CBV alloc + memcpy structure so existing
        // callers don't crash, but we just write a default-PBR blob — anyone
        // who actually relies on per-asset params should route through the
        // Renderer's bindless StructuredBuffer instead.
        const MaterialAsset& asset = entry.asset;

        MaterialGPUData data{};
        data.roughnessMin = 0.0f;  data.roughnessMax = 0.5f;
        data.metalnessMin = 0.0f;  data.metalnessMax = 0.0f;
        data.reflectance  = 0.5f;
        data.baseColor[0] = data.baseColor[1] =
        data.baseColor[2] = data.baseColor[3] = 1.0f;
        data.paramCount   = asset.paramCount;
        data.textureCount = asset.textureCount;

        for (uint32_t i = 0; i < asset.textureCount; ++i)
        {
            const RHI::Texture* tex = texSys.GetTexture(asset.textureSlots[i]);
            data.textureHandleIds[i] = tex ? static_cast<int32_t>(tex->handle_id) : -1;
        }
        for (uint32_t i = asset.textureCount; i < kMaxTextureSlots; ++i)
            data.textureHandleIds[i] = -1;

        if (!entry.cbvValid)
        {
            RHI::GPUBufferDesc cbDesc;
            cbDesc.size       = Align256(sizeof(MaterialGPUData));
            cbDesc.usage      = RHI::Usage::UPLOAD;
            cbDesc.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;

            if (!gfx.CreateBuffer(cbDesc, entry.cbv))
            {
                LOG_ERROR("MaterialSystem: failed to create CBV");
                return;
            }
            entry.cbvValid = true;
        }

        void* mapped = gfx.MapBuffer(entry.cbv);
        if (mapped)
        {
            std::memcpy(mapped, &data, sizeof(MaterialGPUData));
            gfx.UnmapBuffer(entry.cbv);
        }
        else
        {
            LOG_WARNING("MaterialSystem: MapBuffer returned null");
        }
    }

    // -------------------------------------------------------------------------
    // Tick — params only (no TextureSystem available).
    // -------------------------------------------------------------------------

    void MaterialSystem::Tick(IGraphicsDevice& gfx)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Same situation as FlushToCBV: the param-walk over MaterialAsset's
        // generic params[] array no longer maps onto MaterialGPUData (named
        // fields), and the resulting CBV has no live readers. Write defaults
        // so the CBV blob is well-formed; callers who needed per-asset data
        // should route through Renderer::WriteMatSlot.
        MaterialGPUData data{};
        for (auto& entry : m_slots)
        {
            if (!entry.alive || !entry.asset.dirty)
                continue;

            data = {};
            data.roughnessMin = 0.0f;  data.roughnessMax = 0.5f;
            data.metalnessMin = 0.0f;  data.metalnessMax = 0.0f;
            data.reflectance  = 0.5f;
            data.baseColor[0] = data.baseColor[1] =
            data.baseColor[2] = data.baseColor[3] = 1.0f;
            data.paramCount   = entry.asset.paramCount;
            data.textureCount = entry.asset.textureCount;
            for (uint32_t i = 0; i < kMaxTextureSlots; ++i)
                data.textureHandleIds[i] = -1;

            if (!entry.cbvValid)
            {
                RHI::GPUBufferDesc cbDesc;
                cbDesc.size       = Align256(sizeof(MaterialGPUData));
                cbDesc.usage      = RHI::Usage::UPLOAD;
                cbDesc.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
                if (!gfx.CreateBuffer(cbDesc, entry.cbv))
                {
                    LOG_ERROR("MaterialSystem::Tick: failed to create CBV");
                    continue;
                }
                entry.cbvValid = true;
            }

            void* mapped = gfx.MapBuffer(entry.cbv);
            if (mapped)
            {
                std::memcpy(mapped, &data, sizeof(MaterialGPUData));
                gfx.UnmapBuffer(entry.cbv);
                entry.asset.dirty = false;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Tick — full upload including texture handle IDs.
    // -------------------------------------------------------------------------

    void MaterialSystem::Tick(TextureSystem& texSys, IGraphicsDevice& gfx)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& entry : m_slots)
        {
            if (!entry.alive || !entry.asset.dirty)
                continue;

            FlushToCBV(entry, texSys, gfx);

            if (entry.cbvValid)
                entry.asset.dirty = false;
        }
    }

    // -------------------------------------------------------------------------
    // DumpGPUData — fill a flat slot-indexed array for the bindless buffer
    // -------------------------------------------------------------------------

    static void WriteDefaultMaterial(MaterialGPUData& dst)
    {
        dst = {};
        dst.roughnessMin = 1.0f;  dst.roughnessMax = 1.0f;
        dst.metalnessMin = 1.0f;  dst.metalnessMax = 1.0f;
        dst.reflectance  = 0.04f;
        dst.baseColor[0] = dst.baseColor[1] =
        dst.baseColor[2] = dst.baseColor[3] = 1.0f;
        for (auto& id : dst.textureHandleIds) id = -1;
    }

    void MaterialSystem::DumpGPUData(MaterialGPUData* buf, uint32_t maxSlots) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint32_t count = static_cast<uint32_t>(
            std::min(m_slots.size(), static_cast<size_t>(maxSlots)));

        for (uint32_t i = 0; i < count; ++i)
        {
            const MaterialEntry& entry = m_slots[i];
            MaterialGPUData&     dst   = buf[i];

            if (!entry.alive)
            {
                WriteDefaultMaterial(dst);
                continue;
            }

            // Legacy MaterialAsset generic-param flow doesn't map onto the
            // named MaterialGPUData layout; just emit defaults. Renderer's
            // WriteMatSlot is the live path for per-material engine data.
            WriteDefaultMaterial(dst);
            dst.paramCount   = entry.asset.paramCount;
            dst.textureCount = entry.asset.textureCount;
        }

        for (uint32_t i = count; i < maxSlots; ++i)
            WriteDefaultMaterial(buf[i]);
    }

    uint32_t MaterialSystem::GetSlotCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<uint32_t>(m_slots.size());
    }

    // -------------------------------------------------------------------------
    // GetCBV
    // -------------------------------------------------------------------------

    const RHI::GPUBuffer* MaterialSystem::GetCBV(MaterialHandle handle) const
    {
        if (!handle.IsValid()) return nullptr;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle)) return nullptr;
        const MaterialEntry& e = m_slots[handle.Index()];
        return e.cbvValid ? &e.cbv : nullptr;
    }

    // -------------------------------------------------------------------------
    // Shutdown
    // -------------------------------------------------------------------------

    void MaterialSystem::Shutdown(TextureSystem& texSys, IGraphicsDevice& gfx)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& entry : m_slots)
        {
            if (!entry.alive) continue;

            for (uint32_t i = 0; i < entry.asset.textureCount; ++i)
            {
                if (entry.asset.textureSlots[i].IsValid())
                    texSys.Release(entry.asset.textureSlots[i], gfx);
            }

            if (entry.cbvValid)
                gfx.DestroyBuffer(entry.cbv);
        }

        m_slots.clear();
        m_freeList.clear();
    }

} // namespace Resource
