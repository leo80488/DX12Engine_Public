#pragma once

// MeshManager — owns every mesh-side GPU resource the Renderer holds:
//
//   - MeshDescriptorHeap (bindless buffer table + MeshDescriptor StructuredBuffer)
//   - Built-in procedural primitives (Cube / Sphere / Cone)
//   - Billboard quad (shared across all billboard entities)
//   - Per-library bindless VB/IB slots + (library, meshId) → descriptor-slot cache
//
// Method set mirrors what Renderer used to do itself: UploadMesh for procedural
// data, RegisterMeshLibMesh for MeshLibrary-backed streams, plus InitPrimitives
// / InitBillboardQuad for startup. OnWorldClear() bumps a generation counter so
// every cached slot on a MeshLibRef becomes lazily invalid without walking ECS.

#include "Graphics/MeshDescriptorHeap.h"
#include "Graphics/GraphicsStruct.h"
#include "Resource/ProceduralMesh.h"   // MeshData, PrimitiveMeshType
#include "ECS/HierarchyComponents.h"   // MeshLibRef

#include <array>
#include <cstdint>
#include <unordered_map>

class IGraphicsDevice;
namespace Resource { class MeshLibrary; }

class MeshManager
{
public:
    // Per-primitive GPU resources. Public because Renderer draws directly from
    // the primitive pool (m_meshMgr.GetPrimitive(type).meshDescSlot, etc.).
    struct GPUMesh
    {
        RHI::GPUBuffer posBuffer;
        RHI::GPUBuffer normalBuffer;
        RHI::GPUBuffer tangentBuffer;
        RHI::GPUBuffer uvBuffer;
        RHI::GPUBuffer colorBuffer;
        RHI::GPUBuffer indexBuffer;
        uint32_t meshDescSlot = RHI::kInvalidBufferIndex;
        uint32_t indexCount   = 0;
    };

    // Initialise the descriptor heap. Must be called once at startup.
    void Init(IGraphicsDevice& gfx);

    // Pre-generate + upload Cube/Sphere/Cone. Called from Renderer::Compile().
    void InitPrimitives();

    // Upload the shared billboard quad (used by every BillboardComponent). Must
    // run after Init().
    void InitBillboardQuad();

    // Upload a ProceduralMesh::MeshData and register its buffers + MeshDescriptor.
    // On success fills @p out.meshDescSlot and @p out.indexCount.
    bool UploadMesh(const ProceduralMesh::MeshData& data, GPUMesh& out);

    // Register one mesh inside a MeshLibrary. Library shared VB/IB register once
    // into the bindless table; each unique (lib, meshId) gets its own
    // MeshDescriptor slot, cached in a hash map so subsequent lookups are O(1).
    uint32_t RegisterMeshLibMesh(Resource::MeshLibrary& lib, const MeshLibRef& ref);

    // Drop every MeshLibRef cache entry by bumping the generation counter.
    // MeshLibRef::cachedGeneration won't match any more → stale slots ignored.
    void OnWorldClear();

    MeshDescriptorHeap&       GetDescriptorHeap()        { return m_descHeap; }
    const MeshDescriptorHeap& GetDescriptorHeap() const  { return m_descHeap; }

    const GPUMesh& GetPrimitive(int idx) const           { return m_primitives[idx]; }

    uint32_t       GetBillboardMeshDescSlot() const      { return m_billboardMeshDescSlot; }
    const GPUMesh& GetBillboardQuad()         const      { return m_billboardQuad; }

    uint32_t       GetMeshLibDescGeneration() const      { return m_meshLibDescGeneration; }

    // Look up a library's bindless VB/IB slots — populated by RegisterMeshLibMesh.
    // Returns false (out args left at kInvalidBufferIndex) when the library
    // has not been registered yet. Used by the DDGI scene-AS to feed
    // closest-hit's vertex-normal lookup without rebuilding the descriptor
    // heap.
    bool GetLibBindlessSlots(uint32_t libIdx, uint32_t& outVbIdx, uint32_t& outIbIdx) const
    {
        outVbIdx = RHI::kInvalidBufferIndex;
        outIbIdx = RHI::kInvalidBufferIndex;
        auto it = m_meshLibBindless.find(libIdx);
        if (it == m_meshLibBindless.end()) return false;
        outVbIdx = it->second.vbIdx;
        outIbIdx = it->second.ibIdx;
        return outVbIdx != RHI::kInvalidBufferIndex && outIbIdx != RHI::kInvalidBufferIndex;
    }

private:
    IGraphicsDevice*    m_gfx = nullptr;
    MeshDescriptorHeap  m_descHeap;

    std::array<GPUMesh, static_cast<int>(PrimitiveMeshType::Count)> m_primitives;

    // (libSlotIndex << 32 | meshId) → MeshDescriptor slot.
    std::unordered_map<uint64_t, uint32_t> m_meshLibDescCache;
    uint32_t m_meshLibDescGeneration = 1;

    // Per-library bindless VB/IB slots (shared across every mesh in the lib).
    struct MeshLibBindless
    {
        uint32_t vbIdx = RHI::kInvalidBufferIndex;
        uint32_t ibIdx = RHI::kInvalidBufferIndex;
    };
    std::unordered_map<uint32_t, MeshLibBindless> m_meshLibBindless;

    uint32_t m_billboardMeshDescSlot = RHI::kInvalidBufferIndex;
    GPUMesh  m_billboardQuad;
};
