#pragma once

#include "Resource/SystemHandles.h"
#include "Graphics/GraphicsStruct.h"
#include <mutex>
#include <vector>
#include <cstdint>

class IGraphicsDevice;
struct MeshComponent;

namespace Resource
{
    // MeshSystem — manages GPU vertex buffer / index buffer lifetime via a
    // generational slot array.
    //
    // Each Acquire() allocates a new slot (no content-dedup — callers own identity).
    // Release() frees the slot and bumps the generation, making the old handle
    // instantly stale.  IsValid() / GetVertexBuffer() etc. return false/nullptr
    // for stale handles with O(1) generational check and no hash lookup.
    //
    // Vertex layout (interleaved, 32 bytes/vertex):
    //   float3 position  (12 B)
    //   float3 normal    (12 B)
    //   float2 uv0       ( 8 B)
    //
    // Must be called from the main thread.
    class MeshSystem
    {
    public:
        MeshSystem()  = default;
        ~MeshSystem() = default;

        MeshSystem(const MeshSystem&)            = delete;
        MeshSystem& operator=(const MeshSystem&) = delete;

        // Upload MeshComponent CPU data to GPU and return an opaque handle.
        // Returns kInvalidMeshHandle on failure.
        MeshHandle Acquire(const MeshComponent& mesh, IGraphicsDevice& gfx);

        // Direct-blob upload — skips the de-interleave/re-interleave dance
        // that Acquire(MeshComponent) does. The bulk loader reads .imsh
        // blobs whose on-disk layout already matches the GPU layout
        // (interleaved 32-B vertex), so we can upload the raw pointer.
        //
        // Saves two full memcpy passes per mesh (disk-order → three vectors
        // in MeshComponent → interleaved buffer in Acquire). Measurable win
        // at Bistro scale: ~40 ms on 22 k meshes.
        MeshHandle AcquireFromBlob(const void*      vertexData,
                                   uint32_t         vertexCount,
                                   uint32_t         vertexStride,
                                   const uint32_t*  indexData,
                                   uint32_t         indexCount,
                                   IGraphicsDevice& gfx);

        // Free GPU buffers associated with this handle.
        void Release(MeshHandle handle, IGraphicsDevice& gfx);

        bool IsValid(MeshHandle handle) const;

        uint32_t GetIndexCount(MeshHandle handle)  const;
        uint32_t GetVertexCount(MeshHandle handle) const;

        // RHI buffer accessors for BindResource / SetRootBufferSRV.
        const RHI::GPUBuffer* GetVertexBuffer(MeshHandle handle) const;
        const RHI::GPUBuffer* GetIndexBuffer(MeshHandle handle)  const;

        // Release all GPU buffers.  Call before device shutdown.
        void Shutdown(IGraphicsDevice& gfx);

    private:
        struct MeshEntry
        {
            RHI::GPUBuffer vertexBuffer;
            RHI::GPUBuffer indexBuffer;
            uint32_t       vertexCount = 0;
            uint32_t       indexCount  = 0;
            uint32_t       generation  = 0;  // MeshSystem's own generation counter
            bool           alive       = false;
        };

        // Slot pool helpers — caller must hold m_mutex.
        uint32_t AllocSlot();
        void     FreeSlot(uint32_t index);      // bumps generation, clears alive
        bool     IsValidHandle(Handle h) const; // generation + alive check

        mutable std::mutex     m_mutex;
        std::vector<MeshEntry> m_slots;
        std::vector<uint32_t>  m_freeList;
    };
}
