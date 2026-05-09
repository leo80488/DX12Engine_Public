#pragma once

#include "Resource/SystemHandles.h"
#include "Resource/ResourceManager.h"
#include "Resource/TextureResource.h"
#include "Graphics/GraphicsStruct.h"
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

class IGraphicsDevice;

namespace Resource
{
    // TextureSystem — manages GPU Texture lifetime via a generational slot array.
    //
    // Every Acquire() returns a Handle whose generation field enables O(1) stale
    // detection — identical path re-loads after eviction get a new generation, so
    // old handles held by AssetEntry or MaterialAsset are instantly detectable as
    // stale without any extra bookkeeping.
    //
    // Usage pattern (main thread, once per frame):
    //   TextureHandle h = texSys.Acquire(path, rm, gfx);
    //   if (texSys.IsReady(h)) {
    //       gfx.BindResource(*texSys.GetTexture(h), slot, cmd);
    //   }
    //
    // Thread safety: Acquire/Release/IsReady hold m_mutex.
    //               GetTexture is main-thread only (no lock for hot-path perf).
    class TextureSystem
    {
    public:
        TextureSystem()  = default;
        ~TextureSystem() = default;

        TextureSystem(const TextureSystem&)            = delete;
        TextureSystem& operator=(const TextureSystem&) = delete;

        // Acquire a GPU texture for the given path.
        // Same path → same slot, ref-count incremented.  New paths allocate a slot
        // and kick off an async load via ResourceManager.
        TextureHandle Acquire(const std::string& path,
                              ResourceManager&   rm,
                              IGraphicsDevice&   gfx);

        // Decrement ref count.  When count reaches 0, the GPU texture is queued for
        // deferred destruction (safe after the next BeginFrame) and the slot is freed.
        void Release(TextureHandle handle, IGraphicsDevice& gfx);

        // True only when the GPU texture is fully resident.
        bool IsReady(TextureHandle handle) const;

        // Main-thread accessor.  O(1) generational lookup; returns nullptr for
        // invalid, stale, or not-yet-ready handles.
        const RHI::Texture* GetTexture(TextureHandle handle) const;

        // Returns the ResourceManager handle backing this texture, or an
        // invalid Handle if the TextureHandle is stale / unknown. Lets
        // callers reach the CPU-side TextureResource (DirectX::ScratchImage)
        // — used by the terrain collision path to decode an R16_UNORM
        // heightmap into a HeightField. Main-thread only.
        Handle GetResourceManagerHandle(TextureHandle handle) const;

        // Promote RM-ready entries to GPU-ready.
        // Call once per frame on the main thread, AFTER BeginFrame/WaitForPreviousFrame.
        void Tick(ResourceManager& rm, IGraphicsDevice& gfx);

        // Release all GPU textures.  Call once before device shutdown.
        void Shutdown(IGraphicsDevice& gfx);

    private:
        struct TextureEntry
        {
            RHI::Texture texture;
            uint32_t     refCount   = 0;
            Handle       rmHandle;           // IO-layer handle for polling state
            bool         gpuReady   = false;
            uint32_t     generation = 0;     // TextureSystem's own generation
            uint64_t     pathHash   = 0;     // for reverse-lookup on FreeSlot
        };

        // Upload a TextureResource to GPU and mark entry as ready.
        // Caller must hold m_mutex.
        void UploadToGPU(const TextureResource& res, TextureEntry& entry, IGraphicsDevice& gfx);

        // Map DXGI_FORMAT → RHI::Format for the TextureDesc.
        static RHI::Format DxgiToRhiFormat(uint32_t dxgiFmt);

        // FNV-1a 64-bit path hash (same algorithm as ResourceManager).
        static uint64_t HashPath(const std::string& path);

        // Slot pool helpers — caller must hold m_mutex.
        uint32_t AllocSlot();
        void     FreeSlot(uint32_t index);      // bumps generation, removes path mapping
        bool     IsValidHandle(Handle h) const; // generation check

        mutable std::mutex m_mutex;
        std::vector<TextureEntry>              m_slots;
        std::vector<uint32_t>                  m_freeList;
        std::unordered_map<uint64_t, uint32_t> m_pathHashToSlot; // path hash → slot index
        std::vector<RHI::Texture>              m_pendingDestroy; // freed mid-frame, safe after BeginFrame
    };
}
