#include "Graphics/DDGISceneAS.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/MeshManager.h"
#include "Resource/MeshLibrary.h"
#include "Resource/MeshSystem.h"
#include "Resource/ProceduralMesh.h"  // PrimitiveMeshType — MeshHandle.gpuMeshID range check
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/Components.h"
#include "ECS/AnimationComponents.h"  // MeshSkinnedComponent — exclude from DDGI
#include "ECS/DDGIComponents.h"
#include "System/Log.h"

#include <vector>
#include <cstring>

namespace
{
// Mirror of HLSL `struct DDGIInstanceData` in DDGIRayTrace.cs.hlsl. 64 bytes.
struct DDGIInstanceData
{
    float    baseColor[4];     // 16B  (rgb albedo, a unused)
    float    emissive[4];      // 16B  (rgb already multiplied by strength; a unused)
    uint32_t vbBindlessIdx;
    uint32_t ibBindlessIdx;
    uint32_t vertexStart;      // first vertex (count of vertices, not bytes)
    uint32_t indexStart;       // first index  (count of indices,  not bytes)
    int32_t  emissiveTexIdx;   // bindless texture pool index, -1 = none
    uint32_t _pad[3];
};
static_assert(sizeof(DDGIInstanceData) == 64, "DDGIInstanceData must be 64 bytes");
}

using namespace DirectX;

namespace
{

// Engine convention: interleaved 32-byte vertex (float3 pos + float3 nrm + float2 uv).
// MeshLibrary uses this; MeshSystem::AcquireFromBlob assumes the same.
constexpr uint32_t kEngineVertexStride = 32;

// MeshSystem uses uint32 indices; MeshLibrary same. Hard-code R32_UINT.
constexpr DXGI_FORMAT kEngineIndexFormat = DXGI_FORMAT_R32_UINT;
constexpr uint32_t    kEngineIndexBytes  = 4;

// Convert a DirectX::XMFLOAT4X4 (row-major, vector-on-right convention used by
// the engine) into the row-major 3x4 transform that TLAS instances expect.
// XMFLOAT4X4 stores row 0 as the local X-axis, row 3 as the translation —
// but DXR wants column 3 of the 3x4 as translation. We transpose on the fly.
void ToInstanceTransform(const XMFLOAT4X4& m, float t[3][4])
{
    // Engine: position transform is `pos × M`, with translation in row 3.
    // DXR  : transform * (pos, 1), translation in column 3.
    t[0][0] = m._11; t[0][1] = m._21; t[0][2] = m._31; t[0][3] = m._41;
    t[1][0] = m._12; t[1][1] = m._22; t[1][2] = m._32; t[1][3] = m._42;
    t[2][0] = m._13; t[2][1] = m._23; t[2][2] = m._33; t[2][3] = m._43;
}

// Resolve GPU VA for a GPUBuffer through the DX12 backend.
D3D12_GPU_VIRTUAL_ADDRESS BufferGPUVA(GraphicsDX12& gfx, const RHI::GPUBuffer& buf)
{
    ID3D12Resource* r = gfx.GetBufferResource(buf);
    return r ? r->GetGPUVirtualAddress() : 0;
}

} // namespace

// =============================================================================
// Scratch / TLAS resize
// =============================================================================

void DDGISceneAS::EnsureScratch(GraphicsDX12& gfx, uint64_t needed)
{
    if (needed == 0) return;
    if (m_scratch.resource && m_scratch.sizeBytes >= needed) return;
    // Grow with 25% headroom to avoid reallocating on small fluctuations.
    const uint64_t grown = needed + (needed >> 2);
    RT::ScratchBuffer fresh;
    if (RT::AllocateScratch(gfx, grown, fresh))
    {
        // Pipelined frame pacing — the OLD scratch may still be referenced
        // by an AS-build CL submitted in a previous frame. Defer-release
        // tied to the last signaled COMPUTE fence: scratch is compute-only,
        // so once that fence completes no GPU work references it. More
        // precise than the slot-based kFrameCount defer (we're not bound
        // to the backbuffer rotation cycle).
        if (m_scratch.resource)
        {
            constexpr uint32_t kComputeQueue = 1; // RHI::QUEUE_TYPE::COMPUTE
            const uint64_t fv = gfx.GetLastSignaledFenceValue(kComputeQueue);
            gfx.DeferReleaseResource(std::move(m_scratch.resource), kComputeQueue, fv);
        }
        m_scratch = std::move(fresh);
    }
}

void DDGISceneAS::EnsureTLAS(GraphicsDX12& gfx, uint32_t maxInstances)
{
    if (maxInstances == 0) return;
    if (m_tlas.resource && m_tlasCapacity >= maxInstances) return;
    // Round up to power-of-two-ish to limit TLAS reallocations as scene grows.
    uint32_t cap = 1;
    while (cap < maxInstances) cap <<= 1;
    RT::TLAS fresh;
    if (RT::AllocateTLAS(gfx, cap, fresh))
    {
        // Same hazard as EnsureScratch — defer-release the old TLAS + its
        // instance upload buffer instead of letting operator= drop them.
        // The TLAS resource is also referenced by Lighting/SSR via the
        // shader-side TLAS SRV (g_DDGITLAS), so any in-flight read of the
        // old SRV needs the resource alive until that frame's fence clears.
        if (m_tlas.resource)
            gfx.DeferReleaseResource(std::move(m_tlas.resource));
        if (m_tlas.instanceUploadBuffer)
            gfx.DeferReleaseResource(std::move(m_tlas.instanceUploadBuffer));
        m_tlas         = std::move(fresh);
        m_tlasCapacity = cap;
        m_tlasFresh    = false; // freshly allocated → first build cannot be an update
    }
}

void DDGISceneAS::EnsureMaterialBuffer(GraphicsDX12& gfx, uint32_t maxInstances)
{
    if (maxInstances == 0) return;
    if (m_matBuffer[0].IsValid() && m_matCapacity >= maxInstances) return;

    // Free the existing ring (slot-by-slot) before re-allocating at the larger
    // capacity. EnsureMaterialBuffer is called every BuildOrRefit; the
    // already-large-enough fast path above guarantees this only runs on growth.
    // OnWorldClear runs mid-frame so any in-flight CL referencing the OLD
    // buffer slots could still be unsubmitted — but EnsureMaterialBuffer is
    // only ever called from BuildOrRefit, which runs BEFORE its own CL is
    // recorded each frame, so we can synchronously destroy here (no other CL
    // references this resource yet this frame).
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_matBuffer[i].IsValid())
        {
            if (m_matMapped[i]) { gfx.UnmapBuffer(m_matBuffer[i]); m_matMapped[i] = nullptr; }
            gfx.DestroyBuffer(m_matBuffer[i]);
            m_matSrv[i] = 0;
        }
    }

    uint32_t cap = 1;
    while (cap < maxInstances) cap <<= 1;

    RHI::GPUBufferDesc bd{};
    bd.size       = uint64_t(cap) * sizeof(DDGIInstanceData);
    bd.stride     = sizeof(DDGIInstanceData);
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (gfx.CreateBuffer(bd, m_matBuffer[i]))
        {
            m_matMapped[i] = gfx.MapBuffer(m_matBuffer[i]);
            m_matSrv  [i]  = gfx.GetBufferSRVGpuHandle(m_matBuffer[i]);
            if (m_matMapped[i]) std::memset(m_matMapped[i], 0, bd.size);
        }
    }
    m_matCapacity = cap;
}

// =============================================================================
// World clear
// =============================================================================

void DDGISceneAS::OnWorldClear(GraphicsDX12& gfx)
{
    // BLAS cache survives world reload. MeshLibrary path-dedupes + refcounts
    // its slots (project_loadworld_phase12), so same-world reload preserves
    // every (libGen<<32 | libSlot, meshId) cache key and the cached BLAS
    // resource stays valid. Clearing here previously forced 600+ BLAS
    // rebuilds on every reload even though the meshes hadn't changed.
    //
    // Stale entries from world cycles (A→B→A) are reclaimed by PruneStaleBLAS
    // — Renderer calls it from OnWorldClear right after the meshlib
    // two-stage release that actually transitions slots to refCount==0.
    //
    // TLAS resource stays allocated (the buffer is reused across worlds);
    // mark the next build as a non-update rebuild.
    (void)gfx;
    m_tlasFresh         = false;
    m_lastInstanceCount = 0;
}

void DDGISceneAS::PruneStaleBLAS(Resource::MeshLibrary* meshLib, GraphicsDX12& gfx)
{
    if (!meshLib || m_blasCache.empty()) return;

    size_t pruned = 0;
    for (auto it = m_blasCache.begin(); it != m_blasCache.end(); )
    {
        if (it->first.sourceKind != 0)
        {
            // MeshHandle (procedural) — no generation to compare. Skipped:
            // see header comment. Cube/sphere BLAS are tiny.
            ++it;
            continue;
        }

        // Reconstruct the meshlib Handle from CacheKey.libOrSlot.
        const uint32_t slotIdx = static_cast<uint32_t>(it->first.libOrSlot & 0xFFFFFFFFu);
        const uint16_t slotGen = static_cast<uint16_t>(it->first.libOrSlot >> 32);
        const Resource::Handle libHandle = Resource::Handle::Make(
            slotIdx, Resource::ResourceType::MeshLibrary, slotGen);

        if (meshLib->IsValid(libHandle))
        {
            ++it;
            continue;
        }

        // Slot freed or generation bumped — entry is stale. The BLAS resource
        // is a separate ID3D12Resource from the source VB+IB, so it's still
        // GPU-valid and safe to defer-release here.
        if (it->second.resource)
            gfx.DeferReleaseResource(std::move(it->second.resource));
        // An entry pruned mid-compaction still holds the transient size buffers;
        // defer-release them too so they don't leak or free while still in-flight.
        if (it->second.compactSizeUav)
            gfx.DeferReleaseResource(std::move(it->second.compactSizeUav));
        if (it->second.compactSizeReadback)
            gfx.DeferReleaseResource(std::move(it->second.compactSizeReadback));
        it = m_blasCache.erase(it);
        ++pruned;
    }

    if (pruned > 0)
        LOG_INFO("DDGISceneAS: pruned %zu stale BLAS entries", pruned);
}

// =============================================================================
// Per-frame build/refit
// =============================================================================

D3D12_GPU_VIRTUAL_ADDRESS
DDGISceneAS::BuildOrRefit(GraphicsDX12&               gfx,
                          ID3D12GraphicsCommandList4* cmd,
                          World&                      world,
                          Resource::MeshLibrary*      meshLib,
                          Resource::MeshSystem*       meshSys,
                          MeshManager*                meshMgr)
{
    if (!gfx.SupportsDXR() || !cmd) return 0;

    // Advances once per real build/refit; drives the BLAS-compaction readback
    // timing (see m_buildCounter / kCompactReadyDelay below).
    ++m_buildCounter;

    // ---- Phase A: collect candidate static-mesh entities --------------------
    // Static-mesh inclusion rule (Phase 1):
    //   ENTITY MUST HAVE GlobalTransform AND
    //   ((MeshLibRef AND meshLib != nullptr) OR (MeshHandle AND meshSys != nullptr))
    //   AND NOT MeshSkinnedComponent (animated geometry too costly to refit)
    //
    // Phase 2 will honour DDGISceneTagComponent to let users force-include
    // / force-exclude entities; for now the rule above is conservative.
    auto* pGT     = world.GetPool<GlobalTransform>();
    auto* pLibRef = world.GetPool<MeshLibRef>();
    auto* pMesh   = world.GetPool<::MeshHandle>();
    auto* pSkin   = world.GetPool<MeshSkinnedComponent>();
    if (!pGT) return 0;

    // Workspace collected before any AS work runs so we know the instance
    // count up-front (TLAS capacity check) and can build BLASes lazily for
    // unique mesh keys only.
    struct Pending
    {
        CacheKey key;
        XMFLOAT4X4 worldMatrix;
        D3D12_GPU_VIRTUAL_ADDRESS positionVA;
        uint32_t                  positionStride;
        uint32_t                  vertexCount;
        D3D12_GPU_VIRTUAL_ADDRESS indexVA;
        uint32_t                  indexCount;
        DXGI_FORMAT               indexFormat;   // R32_UINT for MeshLibrary, R16_UINT for primitives
        XMFLOAT4                  baseColor;     // packed into per-instance material buf
        XMFLOAT4                  emissive;      // (rgb * strength, 0) — DDGI light contribution
        int32_t                   emissiveTexIdx; // bindless tex pool index, -1 = none
        // Bindless slots + offsets so closest-hit can re-fetch positions for
        // face-normal computation. Zero means "geometry lookup unavailable" —
        // closest-hit falls back to a directional-only NdotL approximation.
        uint32_t                  vbBindlessIdx;
        uint32_t                  ibBindlessIdx;
        uint32_t                  vertexStart;
        uint32_t                  indexStart;
    };
    std::vector<Pending> pending;
    pending.reserve(256);

    auto* pMat = world.GetPool<MaterialComponent>();
    // Default fallback when an entity has no MaterialComponent — neutral grey.
    constexpr XMFLOAT4 kDefaultColor{ 0.5f, 0.5f, 0.5f, 1.0f };

    // ---- MeshLibRef path (P1-P6 scene loader, the bulk of geometry) ---------
    if (pLibRef && meshLib)
    {
        const auto& ents = pLibRef->Entities();
        const auto& data = pLibRef->Data();
        for (size_t i = 0; i < ents.size(); ++i)
        {
            Entity e = ents[i];
            if (!world.IsAlive(e)) continue;
            if (pSkin && pSkin->Get(e)) continue; // skinned excluded

            const GlobalTransform* gt = pGT->Get(e);
            if (!gt) continue;

            const MeshLibRef& ref = data[i];
            if (!ref.IsValid()) continue;

            const auto* entry = meshLib->GetEntry(ref.libHandle, ref.meshId);
            if (!entry || entry->vertexCount == 0 || entry->indexCount == 0) continue;
            const RHI::GPUBuffer* vb = meshLib->GetVertexBuffer(ref.libHandle);
            const RHI::GPUBuffer* ib = meshLib->GetIndexBuffer(ref.libHandle);
            if (!vb || !ib) continue;
            const uint32_t stride = meshLib->GetVertexStride(ref.libHandle);

            D3D12_GPU_VIRTUAL_ADDRESS vbVA = BufferGPUVA(gfx, *vb);
            D3D12_GPU_VIRTUAL_ADDRESS ibVA = BufferGPUVA(gfx, *ib);
            if (!vbVA || !ibVA) continue;

            CacheKey k{};
            k.sourceKind = 0;
            k.libOrSlot  = (uint64_t(ref.libHandle.Generation()) << 32) | ref.libHandle.Index();
            k.meshId     = ref.meshId;

            Pending p{};
            p.key            = k;
            p.worldMatrix    = gt->matrix;
            // DXR BLAS reads vertex N as `positionVA + N * stride`. The
            // engine's MeshLibrary stores absolute indices (0..libTotal-1)
            // in the IB — so positionVA MUST be the library VB start (NOT
            // offset by entry->vertexStart) and vertexCount must cover the
            // largest index value the IB range can produce. Setting
            // positionVA = vbVA + vertexStart*stride double-counts the
            // offset (BLAS adds index value × stride on top), giving an
            // out-of-bounds read → corrupt BLAS → ray traversal hangs the
            // GPU (DEVICE_HUNG TDR). Use the library VB base directly and
            // pass entry->vertexStart + entry->vertexCount as the cap.
            p.positionVA     = vbVA;
            p.positionStride = stride;
            p.vertexCount    = entry->vertexStart + entry->vertexCount;
            p.indexVA        = ibVA + uint64_t(entry->indexStart) * kEngineIndexBytes;
            p.indexCount     = entry->indexCount;
            p.indexFormat    = kEngineIndexFormat;
            // Resolve per-entity base color + emissive for the closest-hit
            // shader. Both fall back to neutral defaults when MaterialComponent
            // is missing. Emissive convention matches GBuffer.ps:
            //   final = emissiveColor.rgb * emissiveColor.w * emissiveTex.rgb
            // The constant component is pre-multiplied here; the texture
            // contribution (if any) is sampled per-hit in the trace shader
            // using the interpolated UV from the VB.
            p.baseColor      = kDefaultColor;
            p.emissive       = { 0.0f, 0.0f, 0.0f, 0.0f };
            p.emissiveTexIdx = -1;
            if (pMat)
                if (const MaterialComponent* mc = pMat->Get(e))
                {
                    p.baseColor = mc->baseColor;
                    const float s = mc->emissiveColor.w;
                    p.emissive  = { mc->emissiveColor.x * s,
                                    mc->emissiveColor.y * s,
                                    mc->emissiveColor.z * s, 0.0f };
                    p.emissiveTexIdx = mc->textures[MaterialComponent::EMISSIVEMAP].bindlessIndex;
                }
            // Bindless slots for vertex/index buffers — closest-hit reads
            // positions through these to compute the geometric face normal.
            // Falls back to invalid (closest-hit detects 0xFFFFFFFFu) when the
            // library hasn't been registered into the bindless heap yet,
            // which happens when a scene loads but no entity has rasterized
            // through the main draw path. The MeshManager populates these
            // slots on the first RegisterMeshLibMesh — Renderer's BuildScene
            // ordering guarantees that runs before BuildOrRefit, so under
            // normal play-mode conditions the slots are always live here.
            p.vbBindlessIdx = RHI::kInvalidBufferIndex;
            p.ibBindlessIdx = RHI::kInvalidBufferIndex;
            if (meshMgr)
                meshMgr->GetLibBindlessSlots(ref.libHandle.Index(),
                                             p.vbBindlessIdx, p.ibBindlessIdx);
            p.vertexStart = entry->vertexStart;
            p.indexStart  = entry->indexStart;
            pending.push_back(p);
        }
    }

    // ---- MeshHandle path (procedural primitives — Cube/Sphere/Cone/Plane/Torus) --
    // ECS MeshHandle stores a slot in MeshManager's primitive pool (bounded by
    // PrimitiveMeshType::Count). The actual GPU buffers are
    // pre-uploaded by MeshManager::InitPrimitives at startup; we read them
    // directly via GetPrimitive(). Procedural primitives use uint16 indices
    // (ProceduralMesh::MeshData::indices) — the index format must be threaded
    // through Pending separately from the MeshLibrary path's R32_UINT.
    if (pMesh && meshMgr)
    {
        constexpr DXGI_FORMAT kPrimIndexFormat = DXGI_FORMAT_R16_UINT;
        constexpr uint32_t    kPrimIndexBytes  = 2;
        constexpr uint32_t    kPrimPosStride   = sizeof(float) * 3; // float3

        const auto& ents = pMesh->Entities();
        const auto& data = pMesh->Data();
        const uint32_t maxPrim = static_cast<uint32_t>(PrimitiveMeshType::Count);
        for (size_t i = 0; i < ents.size(); ++i)
        {
            Entity e = ents[i];
            if (!world.IsAlive(e)) continue;
            if (pSkin && pSkin->Get(e)) continue; // skinned primitives excluded

            const ::MeshHandle& mh = data[i];
            if (!mh.IsValid() || mh.gpuMeshID >= maxPrim) continue;

            const GlobalTransform* gt = pGT->Get(e);
            if (!gt) continue;

            const auto& prim = meshMgr->GetPrimitive(static_cast<int>(mh.gpuMeshID));
            if (!prim.posBuffer.IsValid() || !prim.indexBuffer.IsValid()) continue;
            if (prim.indexCount < 3) continue;

            D3D12_GPU_VIRTUAL_ADDRESS posVA = BufferGPUVA(gfx, prim.posBuffer);
            D3D12_GPU_VIRTUAL_ADDRESS idxVA = BufferGPUVA(gfx, prim.indexBuffer);
            if (!posVA || !idxVA) continue;

            // Vertex count — derived from the position buffer's byte size.
            // Primitives are uploaded as raw float3 streams; size / stride
            // gives the exact count without needing a parallel field on
            // GPUMesh.
            const uint32_t vertexCount = static_cast<uint32_t>(prim.posBuffer.desc.size / kPrimPosStride);

            CacheKey k{};
            k.sourceKind = 1;
            k.libOrSlot  = mh.gpuMeshID; // primitive index identifies the BLAS uniquely
            k.meshId     = 0;

            Pending p{};
            p.key            = k;
            p.worldMatrix    = gt->matrix;
            p.positionVA     = posVA;
            p.positionStride = kPrimPosStride;
            p.vertexCount    = vertexCount;
            p.indexVA        = idxVA;
            p.indexCount     = prim.indexCount;
            p.indexFormat    = kPrimIndexFormat;
            // Per-entity base color + emissive — same fallback as the MeshLibRef path.
            p.baseColor      = kDefaultColor;
            p.emissive       = { 0.0f, 0.0f, 0.0f, 0.0f };
            p.emissiveTexIdx = -1;
            if (pMat)
                if (const MaterialComponent* mc = pMat->Get(e))
                {
                    p.baseColor = mc->baseColor;
                    const float s = mc->emissiveColor.w;
                    p.emissive  = { mc->emissiveColor.x * s,
                                    mc->emissiveColor.y * s,
                                    mc->emissiveColor.z * s, 0.0f };
                    p.emissiveTexIdx = mc->textures[MaterialComponent::EMISSIVEMAP].bindlessIndex;
                }
            // Closest-hit's geometric face-normal lookup needs bindless VB/IB
            // slots. Primitives ARE registered into the bindless heap by
            // MeshManager::UploadMesh, but the slot indices live inside the
            // MeshDescriptor StructuredBuffer, not on GPUMesh — exposing them
            // here would require a new MeshManager getter. Leaving these as
            // invalid sends closest-hit down its directional-NdotL fallback,
            // which produces slightly less accurate bounce light for tagged
            // primitives but never crashes. Acceptable for now; promote to a
            // proper MeshManager accessor if primitives become a meaningful
            // fraction of indirect-bounce contribution.
            p.vbBindlessIdx = RHI::kInvalidBufferIndex;
            p.ibBindlessIdx = RHI::kInvalidBufferIndex;
            p.vertexStart   = 0;
            p.indexStart    = 0;
            pending.push_back(p);
        }
    }
    (void)meshSys;

    if (pending.empty()) return 0;

    // ---- Phase B: build any missing BLAS ------------------------------------
    // Each pending entry references a (mesh) cache key; build BLASes once per
    // unique key. The biggest scratch we'll need is the largest per-BLAS scratch
    // among newly-built BLASes — track that separately from the TLAS scratch.
    //
    // BLAS builds are budgeted per-frame to avoid first-frame TDR on
    // Bistro-scale scenes (thousands of unique meshes would otherwise queue
    // up an unbounded amount of GPU work in a single command list, and even
    // the resource-allocation storm alone can hit Windows' 2-second TDR).
    // Pending unique meshes that don't fit this frame's budget are deferred
    // to subsequent frames; BLAS-less instances simply skip the TLAS until
    // their BLAS arrives. Cap is conservative — tune up if first-frame
    // perf becomes a concern after stability is verified.
    constexpr uint32_t kMaxBLASBuildsPerFrame = 16;
    uint64_t blasScratchMax = 0;
    {
        // Group "build needed" pending entries by key so we don't redo size
        // queries / barrier ceremony per duplicate.
        std::unordered_map<CacheKey, RT::GeometryDesc, CacheKeyHash> needBuild;
        needBuild.reserve(pending.size());

        for (const Pending& p : pending)
        {
            if (m_blasCache.find(p.key) != m_blasCache.end()) continue;
            if (needBuild.find(p.key)   != needBuild.end())   continue;

            RT::GeometryDesc g{};
            g.positionVA     = p.positionVA;
            g.positionStride = p.positionStride;
            g.vertexCount    = p.vertexCount;
            g.indexBufferVA  = p.indexVA;
            g.indexCount     = p.indexCount;
            g.indexFormat    = p.indexFormat; // R32_UINT (lib) or R16_UINT (primitives)
            g.opaque         = true;
            needBuild.emplace(p.key, g);
        }
        const uint32_t totalNeeded = (uint32_t)needBuild.size();
        const uint32_t toBuild     = std::min(totalNeeded, kMaxBLASBuildsPerFrame);
        if (totalNeeded > kMaxBLASBuildsPerFrame)
        {
            static uint32_t s_logCounter = 0;
            if ((++s_logCounter % 60u) == 0)
                LOG_INFO("DDGISceneAS: %u BLAS pending; building %u this frame "
                         "(budget %u, deferring %u to next frame)",
                         totalNeeded, toBuild, kMaxBLASBuildsPerFrame,
                         totalNeeded - toBuild);
        }

        // Size scratch ONCE for BLAS-max + TLAS combined. Reallocating
        // m_scratch after recording AS build commands releases the old
        // resource (still in-flight on GPU) → invalid GPU VA → TDR.
        // Pre-compute everything up front.
        uint32_t sizedSoFar = 0;
        for (const auto& [k, g] : needBuild)
        {
            if (sizedSoFar++ >= toBuild) break;
            uint64_t resSz = 0, scratchSz = 0;
            // allowCompaction=true: prebuild size MUST match the BuildBLAS flags.
            if (!RT::QueryBLASBuildSize(gfx, &g, 1, resSz, scratchSz, /*allowCompaction*/true)) continue;
            blasScratchMax = std::max<uint64_t>(blasScratchMax, scratchSz);
        }
        // Also account for TLAS scratch — query it before any AS-record work.
        EnsureTLAS(gfx, (uint32_t)pending.size());
        const uint64_t totalScratchNeeded = std::max<uint64_t>(blasScratchMax, m_tlas.scratchSize);
        if (totalScratchNeeded > 0) EnsureScratch(gfx, totalScratchNeeded);

        // A BLAS built this call has its COMPACTED_SIZE readback GPU-ready after
        // kFrameCount more BuildOrRefit calls (the slot fence in BeginFrame
        // guarantees this frame's compute work has completed by then). +1 for
        // margin. State 2 (below) reads it and swaps in a shrunk BLAS.
        constexpr uint64_t kCompactReadyDelay = kFrameCount + 1;

        uint32_t builtThisFrame = 0;
        for (const auto& [k, g] : needBuild)
        {
            if (builtThisFrame >= toBuild) break;
            uint64_t resSz = 0, scratchSz = 0;
            if (!RT::QueryBLASBuildSize(gfx, &g, 1, resSz, scratchSz, /*allowCompaction*/true)) continue;
            RT::BLAS blas;
            if (!RT::AllocateBLAS(gfx, resSz, blas)) continue;
            RT::BuildBLAS(gfx, cmd, &g, 1, blas, m_scratch, /*allowCompaction*/true);
            // Queue the compacted-size readback; State 2 shrinks it a few frames
            // later. Failure inside leaves compactState=None → stays full size.
            RT::EmitBLASCompactedSize(gfx, cmd, blas);
            blas.compactReadyCounter = m_buildCounter + kCompactReadyDelay;
            m_blasCache.emplace(k, std::move(blas));
            builtThisFrame++;
        }
    }

    // ---- State 2: compact BLAS whose COMPACTED_SIZE readback is now GPU-ready.
    // Runs BEFORE the TLAS build so any swapped (shrunk) BLAS VA flows into this
    // frame's TLAS instances. CopyRaytracingAccelerationStructure(COMPACT) needs
    // NO scratch, so this does not touch the delicate one-shot m_scratch sizing
    // above. Budgeted like the build loop to bound per-frame GPU work (TDR).
    bool blasSwapped = false;
    {
        constexpr uint32_t kMaxBLASCompactionsPerFrame = 16;
        uint32_t compactedThisFrame = 0;
        for (auto& [k, blas] : m_blasCache)
        {
            if (compactedThisFrame >= kMaxBLASCompactionsPerFrame) break;
            if (blas.compactState != RT::BLAS::CompactState::AwaitingSize) continue;
            if (m_buildCounter < blas.compactReadyCounter) continue; // readback not GPU-ready yet

            uint64_t compactedSize = 0;
            if (blas.compactSizeReadback)
            {
                D3D12_RANGE rr{ 0, sizeof(uint64_t) };
                void* mapped = nullptr;
                if (SUCCEEDED(blas.compactSizeReadback->Map(0, &rr, &mapped)) && mapped)
                {
                    compactedSize = *static_cast<const uint64_t*>(mapped);
                    D3D12_RANGE wr{ 0, 0 };
                    blas.compactSizeReadback->Unmap(0, &wr);
                }
            }

            // Release the transient size buffers regardless of the outcome
            // (slot-based defer: still referenced by the recent emit/copy CL).
            if (blas.compactSizeUav)      gfx.DeferReleaseResource(std::move(blas.compactSizeUav));
            if (blas.compactSizeReadback) gfx.DeferReleaseResource(std::move(blas.compactSizeReadback));

            if (compactedSize > 0 && compactedSize < blas.sizeBytes)
            {
                RT::BLAS compacted;
                if (RT::CompactBLAS(gfx, cmd, blas, compactedSize, compacted))
                {
                    // Original is still referenced by in-flight TLASes (<= kFrameCount
                    // frames) and by this frame's compaction copy — slot-based defer
                    // outlives both (same lifetime class as the TLAS resource).
                    gfx.DeferReleaseResource(std::move(blas.resource));
                    blas.resource     = std::move(compacted.resource);
                    blas.sizeBytes    = compacted.sizeBytes;
                    blas.compactState = RT::BLAS::CompactState::Done;
                    blasSwapped       = true;
                    compactedThisFrame++;
                    continue;
                }
            }
            // No savings (or compaction unavailable) → keep the original, stop retrying.
            blas.compactState = RT::BLAS::CompactState::Done;
        }
    }

    // ---- Phase C: build the TLAS + per-instance material buffer -------------
    EnsureMaterialBuffer(gfx, (uint32_t)pending.size());
    if (!m_tlas.resource) return 0;
    // Scratch was sized in Phase B for max(BLAS, TLAS) — DO NOT EnsureScratch
    // here, that could reallocate while BLAS builds are still queued on `cmd`
    // and reference the old scratch's GPU VA.

    std::vector<RT::TLASInstance> instances;
    instances.reserve(pending.size());
    const uint32_t frameSlot = gfx.GetFrameIndex();
    auto* matDst = (frameSlot < kFrameCount)
        ? static_cast<DDGIInstanceData*>(m_matMapped[frameSlot])
        : nullptr;

    for (const Pending& p : pending)
    {
        auto it = m_blasCache.find(p.key);
        if (it == m_blasCache.end()) continue;

        const uint32_t instIdx = (uint32_t)instances.size();

        RT::TLASInstance inst{};
        ToInstanceTransform(p.worldMatrix, inst.transform);
        inst.instanceID    = instIdx; // Closest-hit reads g_DDGIInstances[InstanceID()]
        inst.instanceMask  = 0xFF;
        inst.hitGroupIndex = 0;
        inst.flags         = 0;
        inst.blasVA        = it->second.GPUAddress();
        instances.push_back(inst);

        if (matDst && instIdx < m_matCapacity)
        {
            DDGIInstanceData d{};
            d.baseColor[0]    = p.baseColor.x;
            d.baseColor[1]    = p.baseColor.y;
            d.baseColor[2]    = p.baseColor.z;
            d.baseColor[3]    = p.baseColor.w;
            d.emissive[0]     = p.emissive.x;
            d.emissive[1]     = p.emissive.y;
            d.emissive[2]     = p.emissive.z;
            d.emissive[3]     = p.emissive.w;
            d.vbBindlessIdx   = p.vbBindlessIdx;
            d.ibBindlessIdx   = p.ibBindlessIdx;
            d.vertexStart     = p.vertexStart;
            d.indexStart      = p.indexStart;
            d.emissiveTexIdx  = p.emissiveTexIdx;
            matDst[instIdx]   = d;
        }
    }
    //// Periodic diagnostic — log instance + cache stats so user can verify
    //// DDGISceneAS actually sees their static geometry. Throttled to ~1 Hz.
    //{
    //    static uint32_t s_logCounter = 0;
    //    if ((++s_logCounter % 60u) == 0)
    //    {
    //        LOG_INFO("DDGISceneAS: pending=%zu instances=%zu blasCache=%zu",
    //                 pending.size(), instances.size(), m_blasCache.size());
    //    }
    //}

    if (instances.empty()) return 0;

    const uint32_t newCount = (uint32_t)instances.size();
    // PERFORM_UPDATE requires the same instance count and same BLAS pointers.
    // Any change in either means we MUST do a full rebuild, otherwise DXR
    // reads garbage on the unchanged-instance assumption and crashes the GPU.
    // A compaction swap this frame changed a BLAS VA; a PERFORM_UPDATE refit
    // (which assumes identical BLAS pointers) would feed a stale/freed VA into
    // the refit → GPU hang. Force a full rebuild on any swap frame.
    const bool canUpdate = m_tlasFresh && (newCount == m_lastInstanceCount) && !blasSwapped;

    RT::WriteTLASInstances(m_tlas, instances.data(), newCount);
    RT::BuildTLAS(gfx, cmd, m_tlas, m_scratch, canUpdate);
    m_tlasFresh         = true;
    m_lastInstanceCount = newCount;

    return m_tlas.GPUAddress();
}

// =============================================================================
// Per-frame accessors
// =============================================================================

const RHI::GPUBuffer* DDGISceneAS::GetInstanceBuffer(GraphicsDX12& gfx) const
{
    const uint32_t s = gfx.GetFrameIndex();
    if (s >= kFrameCount) return nullptr;
    return m_matBuffer[s].IsValid() ? &m_matBuffer[s] : nullptr;
}

uint64_t DDGISceneAS::GetInstanceSrv(GraphicsDX12& gfx) const
{
    const uint32_t s = gfx.GetFrameIndex();
    return (s < kFrameCount) ? m_matSrv[s] : 0;
}
