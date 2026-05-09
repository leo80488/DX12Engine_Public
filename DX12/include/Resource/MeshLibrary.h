#pragma once

#include "Resource/SystemHandles.h"
#include "Graphics/GraphicsStruct.h"

#include <DirectXMath.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class IGraphicsDevice;

namespace Resource
{
    // MeshLibrary — runtime container for a .meshlib file (P1-P6 rewrite).
    //
    // Replaces the legacy "merged .imsh + SubMeshRecord table" model with a
    // first-class per-mesh identity. One .meshlib holds N meshes, all sharing
    // one GPU VB and one GPU IB; each mesh has its own (vertexStart/Count,
    // indexStart/Count, AABB, default material). Scene-graph nodes reference
    // meshes by stable meshId instead of by byte offsets.
    //
    // Lifetime: Load(path) uploads the shared VB+IB to the GPU and caches the
    // entry table. Release(libHandle) frees the GPU buffers and bumps the slot
    // generation so all held MeshLibHandles become instantly stale.
    //
    // Thread safety: Load/Release/Get hold m_mutex. Entry tables returned by
    // pointer are expected to outlive the read site within a single frame —
    // callers must not Release a library while another thread is drawing.
    class MeshLibrary
    {
    public:
        // One mesh in the library, identified by its index into m_slots[libIdx].entries[].
        // Mirrors AssetHeader::MeshLibraryEntry but uses DirectXMath types for
        // ergonomics at the call site.
        struct Entry
        {
            uint32_t          vertexStart        = 0;
            uint32_t          vertexCount        = 0;
            uint32_t          indexStart         = 0;
            uint32_t          indexCount         = 0;
            DirectX::XMFLOAT3 aabbMin            = { 0, 0, 0 };
            DirectX::XMFLOAT3 aabbMax            = { 0, 0, 0 };
            uint32_t          defaultMaterialIdx = 0xFFFFFFFFu;
            uint32_t          flags              = 0;
        };

        MeshLibrary()  = default;
        ~MeshLibrary() = default;

        MeshLibrary(const MeshLibrary&)            = delete;
        MeshLibrary& operator=(const MeshLibrary&) = delete;

        // Load a .meshlib file and upload its shared VB/IB to the GPU.
        // Returns an opaque library handle (ResourceType::MeshLibrary).
        // Returns kInvalidHandle if the file is missing or malformed.
        //
        // Same-path re-load is NOT deduplicated — that's the responsibility of
        // a higher-level cache (SceneInstanceLoader typically loads each
        // library exactly once per scene). Keeps this class single-purpose.
        Handle Load(const std::string& path, IGraphicsDevice& gfx);

        // Free GPU buffers and mark the slot dead. Bumps generation so stale
        // handles fail IsValid(). Safe to call with kInvalidHandle (no-op).
        void Release(Handle libHandle, IGraphicsDevice& gfx);

        bool IsValid(Handle libHandle) const;

        // Mesh count in this library (number of valid Entry slots).
        uint32_t GetMeshCount(Handle libHandle) const;

        // Per-mesh metadata. Returns nullptr for invalid/stale handle or
        // out-of-range meshId. Pointer is stable until Release().
        const Entry* GetEntry(Handle libHandle, uint32_t meshId) const;

        // Raw-byte library-wide pool access — used by Renderer to register
        // one bindless SRV per library (not per mesh), since every mesh
        // shares the same VB/IB.
        const RHI::GPUBuffer* GetVertexBuffer(Handle libHandle) const;
        const RHI::GPUBuffer* GetIndexBuffer (Handle libHandle) const;

        // Library-wide vertex stride (32 for legacy, 48 for new with-tangent
        // format). Exposed so Renderer can build MeshDescriptor.byteStride
        // without the caller knowing the format version.
        uint32_t GetVertexStride(Handle libHandle) const;

        // Whether the library's interleaved VB carries a per-vertex tangent
        // (float4 at byte offset 32). Drives MeshManager's choice of binding
        // the tangent stream — legacy 32-byte libraries return false and let
        // GBuffer.vs synthesise the basis from the normal alone.
        bool GetHasTangent(Handle libHandle) const;

        // Release every loaded library. Call before IGraphicsDevice::Shutdown.
        void Shutdown(IGraphicsDevice& gfx);

        // Enumerate all currently-alive library handles. Used by the editor
        // UI to list loaded libraries. Copies handles — the returned vector
        // is safe to outlive the mutex window.
        std::vector<Handle> GetLoadedHandles() const;

        // Library-wide totals (used by the editor's MeshLibrary panel).
        // Returns (vertexCount, indexCount) summed across every entry, or
        // zero if the library handle is invalid.
        struct TotalsView
        {
            uint32_t vertexCount = 0;
            uint32_t indexCount  = 0;
        };
        TotalsView GetTotals(Handle libHandle) const;

        // Source path captured when Load() succeeded. Empty string for
        // invalid/stale handles. Used only by the editor — the runtime never
        // needs the path after load.
        std::string GetSourcePath(Handle libHandle) const;

    private:
        struct Slot
        {
            std::vector<Entry> entries;
            RHI::GPUBuffer     vb;
            RHI::GPUBuffer     ib;
            std::string        sourcePath;
            uint32_t           vertexCountTotal = 0;
            uint32_t           indexCountTotal  = 0;
            uint32_t           vertexStride     = 32;
            uint32_t           generation       = 0;   // 0 = never issued
            bool               alive            = false;
            // Set when the on-disk metadata flags bit MESHLIB_FLAG_HAS_TANGENT.
            // Drives whether MeshManager registers the tangent stream in the
            // MeshDescriptor (offset 32, format Float4) — legacy 32-byte
            // libraries leave this false and rely on GBuffer.vs's synthesised
            // basis fallback.
            bool               hasTangent       = false;
        };

        // Slot pool helpers — caller must hold m_mutex.
        uint32_t AllocSlot();
        void     FreeSlot(uint32_t index);
        bool     IsValidHandle(Handle h) const;

        // Resolve handle → slot pointer (const). Returns nullptr for invalid
        // handle. Caller must hold m_mutex.
        const Slot* GetSlot(Handle h) const;

        mutable std::mutex m_mutex;
        std::vector<Slot>   m_slots;
        std::vector<uint32_t> m_freeList;
    };
}
