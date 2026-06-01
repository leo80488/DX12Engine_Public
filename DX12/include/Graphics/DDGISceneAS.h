#pragma once

// DDGISceneAS — DDGI-only acceleration-structure cache.
//
// Owns:
//   - Per-mesh BLAS pool (keyed by library handle + meshId for MeshLibRef
//     entities; keyed by MeshSystem handle for MeshHandle entities).
//   - One scene-wide TLAS that's rebuilt each frame from active static
//     entities found in the world.
//   - A reusable scratch buffer (sized for the largest build seen this run).
//
// Lifecycle:
//   OnWorldClear()                  — drop all BLAS resources + clear instance map.
//   BuildOrRefit(gfx, world, ...)   — record AS build onto a CL. First call after a
//                                    cleared world rebuilds; subsequent calls do a
//                                    transform-only TLAS update (PERFORM_UPDATE)
//                                    when only world matrices changed.
//
// Layering:
//   Lives in the DX12 backend layer because RT::BLAS / RT::TLAS are DXR types.
//   Pure Renderer-internal helper; no other pass touches this class directly.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/Raytracing.h"
#include "Graphics/GraphicsStruct.h"
#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"

#include <unordered_map>
#include <cstdint>

class GraphicsDX12;
class World;
class MeshManager;
namespace Resource { class MeshLibrary; class MeshSystem; }

class DDGISceneAS
{
public:
    // BLAS cache survives world reload now (path-deduped MeshLibrary keeps
    // shared meshlib slots alive across reloads, so cache keys stay valid).
    // Only TLAS freshness is reset here. See PruneStaleBLAS for the
    // memory-recovery pass that releases entries whose underlying meshlib
    // slot has been freed by Renderer's two-stage release.
    void OnWorldClear(GraphicsDX12& gfx);

    // Sweep the BLAS cache and defer-release entries whose MeshLibrary slot
    // is no longer alive (refCount==0 → FreeSlot bumped generation). Without
    // this, A→B→A world cycling leaks ~200 MB per round-trip in Bistro-scale
    // worlds because m_blasCache's ComPtr keeps the BLAS resources resident
    // even after the source VB+IB went away. Call from Renderer::OnWorldClear
    // AFTER the m_meshLibsPendingRelease drain — that's the moment a slot
    // can transition refCount 1→0 and become stale-detectable here.
    //
    // sourceKind==1 (MeshHandle / procedural) entries are not pruned by this
    // pass because MeshHandle stores only a raw uint32 ID, no generation —
    // staleness isn't observable. Procedural BLAS are tiny (cube/sphere) so
    // the leak is bounded.
    void PruneStaleBLAS(Resource::MeshLibrary* meshLib, GraphicsDX12& gfx);

    // Walk @p world for static-mesh entities (anything carrying MeshLibRef or
    // MeshHandle + GlobalTransform; skinned meshes are excluded by the
    // tag-presence checks). Build any missing BLAS, refit / rebuild the TLAS,
    // then record everything onto @p cmd. The CL must be a graphics or
    // compute queue list.
    //
    // Returns the TLAS GPU VA on success, 0 if the build was skipped (no
    // static geometry, DXR unsupported, or unrecoverable error).
    D3D12_GPU_VIRTUAL_ADDRESS BuildOrRefit(GraphicsDX12&                       gfx,
                                           ID3D12GraphicsCommandList4*         cmd,
                                           World&                              world,
                                           Resource::MeshLibrary*              meshLib,
                                           Resource::MeshSystem*               meshSys,
                                           MeshManager*                        meshMgr);

    // True when the cached TLAS resource is allocated and last-built non-empty.
    bool IsValid() const { return m_tlas.resource != nullptr && m_tlas.instanceCount > 0; }

    // Direct accessor for the TLAS GPU VA — same value returned by the last
    // BuildOrRefit() call. 0 when no TLAS built yet.
    D3D12_GPU_VIRTUAL_ADDRESS GetTLASAddress() const { return m_tlas.GPUAddress(); }

    // Per-instance data buffer — closest-hit reads g_DDGIInstances[InstanceID()]
    // for albedo + emissive + bindless VB/IB slots + geometry offsets. Format
    // is the 48-byte struct DDGIInstanceData (mirror in DDGIRayTrace.cs.hlsl).
    // Triple-buffered (per-frame written). The accessor returns the current
    // frame's slot — pass the gfx ref so we can index by GetFrameIndex().
    // Returns nullptr before the first BuildOrRefit call.
    const RHI::GPUBuffer* GetInstanceBuffer(GraphicsDX12& gfx) const;
    // Backwards-compat alias — old call sites used this name.
    const RHI::GPUBuffer* GetMaterialBuffer(GraphicsDX12& gfx) const { return GetInstanceBuffer(gfx); }
    // SRV GPU handle of the per-frame instance buffer.
    uint64_t              GetInstanceSrv(GraphicsDX12& gfx) const;

private:
    // BLAS cache key — combines source kind + identifier.
    struct CacheKey
    {
        uint32_t sourceKind; // 0 = MeshLibRef, 1 = MeshHandle
        uint64_t libOrSlot;  // MeshLibRef: (libGen << 32) | libSlot
                             // MeshHandle: (gen << 32) | slot
        uint32_t meshId;     // MeshLibRef: entry index; MeshHandle: 0
        bool operator==(const CacheKey& o) const noexcept
        { return sourceKind == o.sourceKind && libOrSlot == o.libOrSlot && meshId == o.meshId; }
    };
    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const noexcept
        {
            uint64_t h = k.libOrSlot * 0x9E3779B97F4A7C15ull;
            h ^= (uint64_t)k.meshId * 0xBF58476D1CE4E5B9ull;
            h ^= (uint64_t)k.sourceKind << 56;
            return (size_t)h;
        }
    };

    std::unordered_map<CacheKey, RT::BLAS, CacheKeyHash> m_blasCache;
    RT::TLAS                                              m_tlas;
    RT::ScratchBuffer                                     m_scratch;
    uint32_t                                              m_tlasCapacity = 0;
    bool                                                  m_tlasFresh    = false; // false → next build is non-update
    // Last frame's instance count — DXR PERFORM_UPDATE requires identical
    // instance count + BLAS pointers. When the count changes (more BLASes
    // finished building this frame, or scene churn), force a full rebuild.
    uint32_t                                              m_lastInstanceCount = 0;

    // Monotonic BuildOrRefit call counter, used to time BLAS-compaction
    // readbacks: a COMPACTED_SIZE query recorded this call is GPU-ready after
    // kFrameCount more calls (the swap-chain slot fence guarantees the build
    // frame's compute work has completed by then). Drives the 2-state
    // compact-on-a-later-frame machine without any cross-queue fence plumbing.
    uint64_t                                              m_buildCounter = 0;

    // Per-DDGI-instance material buffer (UPLOAD heap, StructuredBuffer<float4>).
    // One entry per TLAS instance, indexed by InstanceID() in HLSL. Sized to
    // m_tlasCapacity and kept in sync with the TLAS rebuild.
    // Triple-buffered: written every frame in BuildOrRefit. Each slot is
    // resized independently when EnsureMaterialBuffer grows the capacity.
    static constexpr uint32_t kFrameCount = 3;
    RHI::GPUBuffer                                        m_matBuffer[kFrameCount];
    void*                                                 m_matMapped[kFrameCount] = {};
    uint64_t                                              m_matSrv   [kFrameCount] = {};
    uint32_t                                              m_matCapacity = 0;

    // Grow scratch to at least @p needed bytes. Reallocates only on growth.
    void EnsureScratch(GraphicsDX12& gfx, uint64_t needed);

    // Allocate / grow the TLAS to fit @p maxInstances. Pure resize, no build.
    void EnsureTLAS(GraphicsDX12& gfx, uint32_t maxInstances);

    // Allocate / grow the per-instance material buffer to fit @p maxInstances.
    void EnsureMaterialBuffer(GraphicsDX12& gfx, uint32_t maxInstances);
};
