#pragma once

#include "Resource/SystemHandles.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Graphics/GraphicsStruct.h"
#include <mutex>
#include <vector>
#include <string>

class IGraphicsDevice;

namespace Resource
{
    // MaterialSystem — owns MaterialAsset CPU structs and drives per-frame GPU uploads
    // via a generational slot array.
    //
    // Responsibility:
    //   - Allocates and holds MaterialAsset objects (CPU-side PBR params + texture slots).
    //   - Resolves per-slot texture paths → TextureHandles via TextureSystem.
    //   - Tick(): for every dirty MaterialAsset, uploads params to an UPLOAD-heap CBV
    //             and clears the dirty flag.
    //
    // Thread safety: Create/Destroy/Get hold m_mutex.
    //               Tick must only be called from the main thread.
    class MaterialSystem
    {
    public:
        MaterialSystem()  = default;
        ~MaterialSystem() = default;

        MaterialSystem(const MaterialSystem&)            = delete;
        MaterialSystem& operator=(const MaterialSystem&) = delete;

        // Allocate a new default MaterialAsset and return its handle.
        MaterialHandle Create();

        // Free a MaterialAsset and release its texture refs.
        void Destroy(MaterialHandle handle, TextureSystem& texSys, IGraphicsDevice& gfx);

        // CPU-side MaterialAsset access.  Returns nullptr for invalid/stale handles.
        MaterialAsset*       Get(MaterialHandle handle);
        const MaterialAsset* Get(MaterialHandle handle) const;

        // Resolve per-slot texture paths → TextureHandles via TextureSystem.
        void ResolveTextures(MaterialHandle    handle,
                             const std::string paths[],
                             uint32_t          pathCount,
                             TextureSystem&    texSys,
                             ResourceManager&  rm,
                             IGraphicsDevice&  gfx);

        // Per-frame main-thread pump: upload all dirty MaterialAssets to their CBVs.
        void Tick(IGraphicsDevice& gfx);
        void Tick(TextureSystem& texSys, IGraphicsDevice& gfx);

        // Return the GPU constant buffer for a material (nullptr if not yet uploaded).
        const RHI::GPUBuffer* GetCBV(MaterialHandle handle) const;

        // Fill a flat array (indexed by slot) with GPU-ready material data.
        // Used by Renderer to build a per-frame StructuredBuffer for bindless access.
        // Slots without a live material are filled with default PBR values.
        void DumpGPUData(MaterialGPUData* buf, uint32_t maxSlots) const;

        // Number of allocated slots (= capacity needed for DumpGPUData).
        uint32_t GetSlotCount() const;

        // Destroy all materials.  Call before device shutdown.
        void Shutdown(TextureSystem& texSys, IGraphicsDevice& gfx);

    private:
        struct MaterialEntry
        {
            MaterialAsset  asset;
            RHI::GPUBuffer cbv;
            bool           cbvValid   = false;
            uint32_t       generation = 0;  // MaterialSystem's own generation counter
            bool           alive      = false;
        };

        // Build the GPU-ready mirror of a MaterialAsset and write it into the CBV.
        // Caller must hold m_mutex.
        void FlushToCBV(MaterialEntry& entry, const TextureSystem& texSys, IGraphicsDevice& gfx);

        // Slot pool helpers — caller must hold m_mutex.
        uint32_t AllocSlot();
        void     FreeSlot(uint32_t index);      // bumps generation, clears alive
        bool     IsValidHandle(Handle h) const; // generation + alive check

        mutable std::mutex         m_mutex;
        std::vector<MaterialEntry> m_slots;
        std::vector<uint32_t>      m_freeList;
    };
}
