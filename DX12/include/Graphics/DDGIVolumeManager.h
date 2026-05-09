#pragma once

// DDGIVolumeManager — owns every per-volume GPU resource for DDGI.
//
// Atlas layout (all atlases tile probes as a 2D grid: probeCountX × probeCountY*probeCountZ):
//
//   Irradiance Atlas  (R11G11B10_FLOAT) — 6×6 octahedral patch + 1-texel border on
//                                         each side = 8×8 per probe. Stored value
//                                         is `sqrt(irradiance)` (perceptual blend).
//   Depth Atlas       (R16G16_FLOAT)    — 16×16 octahedral patch + 1-texel border
//                                         = 18×18 per probe. .x = mean distance,
//                                         .y = distance² (chebyshev visibility).
//   Probe Data        (RWStructuredBuffer<ProbeData>) — per-probe relocation offset,
//                                                      classification state, stats.
//   Ray Data          (RWStructuredBuffer<float4>)    — per-frame trace results,
//                                                      indexed [probe][ray]. RGB =
//                                                      radiance, A = hit distance.
//   Volume Constants  (CBV)                            — origin/extent/probeCounts/
//                                                      randomRotation/biases/etc.
//
// Resource lifecycle:
//   Allocate(volume, runtimeOut)  — first-time create or full reallocation when
//                                   probeCounts / raysPerProbe change.
//   Tick(world, settings)         — per-frame: compute random rotation, update
//                                   the constant buffer, feed runtime snapshot.
//   Free(slot)                    — release on volume destroy.
//
// Bindings:
//   Lighting.ps.hlsl reads each volume's irradiance/depth atlas + probe data
//   buffer. The Renderer uploads a packed StructuredBuffer<DDGIVolumeGPUDesc>
//   that mirrors the volume params + per-volume atlas slot indices into the
//   bindless texture table — single root parameter, supports up to kMaxVolumes.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/GraphicsStruct.h"
#include "Graphics/DescriptorHeapAllocator.h"
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

class IGraphicsDevice;
class GraphicsDX12;
struct DDGIVolumeComponent;
struct DDGIVolumeRuntimeComponent;

namespace DDGI
{

// Octahedral patch sizes (probe-local). Border adds +1 texel each side.
constexpr uint32_t kIrradianceProbeSize = 6;
constexpr uint32_t kIrradianceProbeStride = kIrradianceProbeSize + 2; // 8
constexpr uint32_t kDepthProbeSize       = 16;
constexpr uint32_t kDepthProbeStride     = kDepthProbeSize + 2;       // 18

// Engine-wide ceiling on simultaneously-active DDGI volumes — sized for indoor
// scenes where one or two volumes per "room" cover the camera at any time.
// Larger ceilings inflate the bindless slot pressure on the Lighting PS, so
// keep this small unless a future open-world cascade scheme demands more.
constexpr uint32_t kMaxVolumes = 4;

// Mirror of DDGICommon.hlsli #define — adaptive ray count is stored as bucket
// count, where 1 bucket = DDGI_RAY_BUCKET_COUNT rays.
constexpr uint32_t DDGI_RAY_BUCKET_COUNT = 4;

// GPU-visible descriptor mirroring DDGIVolumeComponent + atlas slot indices.
// 1:1 with HLSL `struct DDGIVolumeGPU` in DDGICommon.hlsli (keep in sync).
struct VolumeGPUDesc
{
    DirectX::XMFLOAT3 origin;       float padA = 0;
    DirectX::XMFLOAT3 extent;       float padB = 0;
    DirectX::XMFLOAT3 probeSpacing; float padC = 0;
    uint32_t          probeCountsX, probeCountsY, probeCountsZ;
    uint32_t          raysPerProbe;
    float             hysteresis;
    float             normalBias;
    float             viewBias;
    float             boundaryFadeRatio;

    uint32_t          flags;          // bit 0: enabled, bit 1: relocation, bit 2: classification, bit 3: overwrite atlas
    uint32_t          _padFlag0 = 0, _padFlag1 = 0, _padFlag2 = 0;

    DirectX::XMFLOAT4 randomRotation0; // float3x3 packed as 3 float4s — last column unused
    DirectX::XMFLOAT4 randomRotation1;
    DirectX::XMFLOAT4 randomRotation2;

    DirectX::XMFLOAT3 diffuseTint;   float diffuseScale = 1.0f;

    // Trace-time lighting integration. The trace CS picks a random light per
    // ray from the cluster GPULight buffer (bound externally via DDGIPass at
    // t7), evaluates direct lighting + shadow ray, and importance-samples by
    // multiplying by lightCount.
    //
    //   lightCount  — number of GPULight entries in the bound buffer.
    //   frameIndex  — per-volume frame counter (multi-bounce branch + RNG seed).
    uint32_t          lightCount   = 0;
    uint32_t          frameIndex   = 0;
    uint32_t          _padFI0      = 0;
    uint32_t          _padFI1      = 0;
};

// Per-probe state stored in ProbeData buffer (kept compact — written by classify
// + relocate compute passes, read by sampling).
struct ProbeData
{
    DirectX::XMFLOAT3 offset;       // relocation offset (Phase 3)
    uint32_t          state;        // 0 = ACTIVE (default), 1 = INACTIVE.
                                    // Encoded so zero-init buffer means
                                    // every probe is active without needing
                                    // a separate init pass.
};

class DDGIVolumeManager
{
public:
    bool Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // Allocate (or reallocate on size change) GPU resources for one volume.
    // Stamps runtime.volumeSlot + the cached probeCounts/raysPerProbe.
    bool AllocateOrUpdate(IGraphicsDevice& gfx,
                          const DDGIVolumeComponent& volume,
                          DDGIVolumeRuntimeComponent& runtime);

    // Free the resources occupied by @p slot. Safe with ~0u (no-op).
    void Free(IGraphicsDevice& gfx, uint32_t slot);

    // Free every allocated slot whose index is NOT in @p claimedSlots. Caller
    // passes the per-frame list of slots referenced by live runtime components;
    // anything else is treated as orphaned (entity destroyed, scene reload,
    // re-created runtime component, etc.) and released.
    //
    // Required to prevent the "stale slot 0 + new slot 1" duplication that
    // makes the lighting pass read from one slot's buffer using another
    // slot's probe-count layout (out-of-bounds → garbage SH irradiance).
    void FreeUnclaimedSlots(IGraphicsDevice& gfx,
                            const uint32_t* claimedSlots,
                            uint32_t        claimedCount);

    // Per-frame: pump random rotation, refresh the volume CB upload buffer,
    // pack the engine-wide VolumeGPUDesc array. Caller must have called
    // AllocateOrUpdate for every active volume earlier this frame.
    //
    // @p lightCount comes from ClusterPass::GetLightCount() — the trace CS
    // picks a random light index in [0..lightCount-1] per ray.
    void Tick(IGraphicsDevice& gfx,
              const DDGIVolumeComponent* const* volumes,
              DDGIVolumeRuntimeComponent* const* runtimes,
              uint32_t volumeCount,
              uint32_t lightCount);

    // SRV GPU handle of the packed StructuredBuffer<VolumeGPUDesc> — bound by
    // Lighting.ps + DDGI passes. Returns 0 before the first Tick().
    uint64_t  GetVolumeBufferSrv() const { return m_volumeBufferSrv; }

    // Multi-volume descriptor-table base GPU handles. Each table holds
    // kMaxVolumes contiguous SRVs in slot-index order — Lighting.ps binds these
    // as `register(tN)` arrays of length DDGI_MAX_VOLUMES and indexes by the
    // shader-side volume loop counter. Slots without an allocated volume hold
    // null SRVs (valid descriptors, never read because ddgiVolumeCount gates
    // the loop). Returns 0 before Init() succeeds.
    uint64_t  GetProbeSHTableGpu()    const { return m_probeSHTableGpu; }
    uint64_t  GetDepthTableGpu()      const { return m_depthTableGpu; }
    uint64_t  GetProbeDataTableGpu()  const { return m_probeDataTableGpu; }

    // Per-volume SRVs. ProbeSH replaces the old irradiance atlas — L1 SH
    // coefficients per probe, 48 B/probe, sampled via SH evaluation rather
    // than bilinear texture lookup.
    uint64_t  GetProbeSHSrv        (uint32_t slot) const;
    uint64_t  GetDepthAtlasSrv     (uint32_t slot) const;
    uint64_t  GetProbeDataSrv      (uint32_t slot) const;
    uint64_t  GetProbeSHUav        (uint32_t slot) const;
    uint64_t  GetDepthAtlasUav     (uint32_t slot) const;
    uint64_t  GetRayDataUav        (uint32_t slot) const;
    uint64_t  GetRayDataSrv        (uint32_t slot) const;
    uint64_t  GetProbeDataUav      (uint32_t slot) const;
    uint64_t  GetVarianceUav       (uint32_t slot) const;
    uint64_t  GetRayCountUav       (uint32_t slot) const;  // adaptive bucket counts
    uint64_t  GetRayAllocUav       (uint32_t slot) const;  // total + ray descriptors
    uint64_t  GetDispatchArgsUav   (uint32_t slot) const;  // D3D12_DISPATCH_ARGUMENTS

    // Underlying GPUBuffer for the dispatch-args buffer. DDGIPass needs the raw
    // ID3D12Resource* to feed ExecuteIndirect's args parameter, so the manager
    // exposes the buffer (not just its UAV handle) for that one call site.
    const RHI::GPUBuffer* GetDispatchArgsBuffer(uint32_t slot) const;

    // Underlying SH probe buffer — DDGIPass needs the raw ID3D12Resource* to
    // issue its NPSR ↔ UAV transitions per frame.
    const RHI::GPUBuffer* GetProbeSHBuffer(uint32_t slot) const;

    // Underlying probe-data buffer (DDGIProbeData per probe). DDGIPass needs
    // the raw resource to flip NPSR (trace's multi-bounce SRV read) → UAV
    // (relocation CS write) within the same frame.
    const RHI::GPUBuffer* GetProbeDataBuffer(uint32_t slot) const;

    // Per-volume CB GPU virtual address. The DDGI passes bind this directly via
    // SetComputeRootCBV on the per-volume dispatch.
    uint64_t  GetVolumeCBVGpuAddress(uint32_t slot) const;

    // Per-volume CB underlying GPUBuffer — used by DDGIPass to query the
    // backend for the resource's GPU VA at bind time (the manager doesn't
    // store the VA because the backend doesn't expose it on IGraphicsDevice;
    // GraphicsDX12::GetBufferResource() resolves it for callers in the DX12
    // layer).
    const RHI::GPUBuffer* GetVolumeCB(uint32_t slot) const;

    // Resource state — passes use this to push the right barrier when toggling
    // between UAV (relight / trace writes) and SRV (sampling read in Lighting).
    //
    // @p computeQueueState — when true, the SRV transition target is
    // NON_PIXEL_SHADER_RESOURCE only (the only SRV state valid on a COMPUTE
    // command list; the full PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
    // mask used by graphics-queue lighting reads is invalid on compute and
    // would trip the D3D12 debug layer / risk a TDR). Caller is responsible
    // for re-promoting NON_PIXEL → full SHADER_RESOURCE on the graphics queue
    // before LightingPass binds the atlas.
    enum class AtlasState : uint8_t { UAV, SRV };
    void      TransitionVolumeAtlases(IGraphicsDevice& gfx,
                                      RHI::CommandList cmd,
                                      uint32_t slot, AtlasState newState,
                                      bool computeQueueState = false);

    // Graphics-queue helper: promote each ACTIVE slot's depth atlas + SH probe
    // buffer from the NON_PIXEL_SHADER_RESOURCE state left behind by the
    // compute-queue DDGI pipeline → the full PIXEL | NON_PIXEL shader-resource
    // state expected by LightingPass's pixel-shader reads. Must be called on a
    // GRAPHICS command list, after the cross-queue fence guarantees the DDGI
    // compute work has completed. No-op when DDGI is running on the graphics
    // queue (atlases already in the full state).
    void      PromoteAtlasesForGraphicsQueue(IGraphicsDevice& gfx,
                                             RHI::CommandList graphicsCmd);

    // Probe count helpers for sizing dispatches.
    uint32_t  GetProbeCount(uint32_t slot) const;
    uint32_t  GetRaysPerProbe(uint32_t slot) const;
    uint32_t  GetDepthAtlasWidth      (uint32_t slot) const;
    uint32_t  GetDepthAtlasHeight     (uint32_t slot) const;

    // Number of currently-allocated volumes (slot count up to kMaxVolumes).
    uint32_t  GetActiveVolumeCount() const { return m_activeCount; }

private:
    struct VolumeResources
    {
        bool          allocated = false;
        uint32_t      probeCountsX = 0, probeCountsY = 0, probeCountsZ = 0;
        uint32_t      raysPerProbe = 0;
        AtlasState    atlasState   = AtlasState::SRV; // matches CreateTexture default
        // True when the most-recent SRV transition restricted the depth atlas
        // to NON_PIXEL_SHADER_RESOURCE only (i.e. DDGI ran on a compute queue
        // this frame). PromoteAtlasesForGraphicsQueue lifts it back to the
        // full PIXEL | NON_PIXEL state on the graphics queue.
        bool          atlasInComputeOnlyState = false;

        RHI::GPUBuffer probeSHBuffer;     // RWStructuredBuffer<DDGIProbeSH> — L1 SH per probe.
                                          // Replaces the old irradiance atlas.
        RHI::Texture   depthAtlas;        // R16G16F
        RHI::GPUBuffer probeDataBuffer;   // RWStructuredBuffer<ProbeData>
        RHI::GPUBuffer rayDataBuffer;     // RWStructuredBuffer<float4>  (radiance + dist)
        RHI::GPUBuffer varianceBuffer;    // RWStructuredBuffer<DDGIVarianceData> — per-texel
                                          // multi-scale mean estimator state (firefly + adaptive blend).
        RHI::GPUBuffer rayCountBuffer;    // RWStructuredBuffer<uint> — per-probe bucket count
                                          // (count*BUCKET = actual rays for this frame).
        RHI::GPUBuffer rayAllocBuffer;    // RWStructuredBuffer<uint> — [0] = total rays,
                                          // [1..] = packed (probeIdx | rayIdx<<20).
        RHI::GPUBuffer dispatchArgsBuffer;// 16 B — [0..2] = D3D12_DISPATCH_ARGUMENTS for the
                                          // trace's ExecuteIndirect. Toggles UAV↔INDIRECT_ARGUMENT.
        RHI::GPUBuffer volumeCB;          // CB upload-mapped, mirrors VolumeGPUDesc subset

        // Cached SRV/UAV handles for hot-path lookups.
        uint64_t       probeSHSrv         = 0;
        uint64_t       depthAtlasSrv      = 0;
        uint64_t       probeDataSrv       = 0;
        uint64_t       rayDataSrv         = 0;
        uint64_t       probeSHUav         = 0;
        uint64_t       depthAtlasUav      = 0;
        uint64_t       probeDataUav       = 0;
        uint64_t       rayDataUav         = 0;
        uint64_t       varianceUav        = 0;
        uint64_t       rayCountUav        = 0;
        uint64_t       rayAllocUav        = 0;
        uint64_t       dispatchArgsUav    = 0;

        // Persistently-mapped CBV pointer (UPLOAD heap).
        void*          cbvMapped          = nullptr;
        uint64_t       cbvGpuAddress      = 0;
    };

    void DestroyVolume(IGraphicsDevice& gfx, VolumeResources& res);
    bool CreateAtlasesForVolume(IGraphicsDevice& gfx,
                                const DDGIVolumeComponent& v,
                                VolumeResources& res);

    VolumeResources m_volumes[kMaxVolumes];
    uint32_t        m_activeCount = 0;

    // Engine-wide volume descriptor buffer (kMaxVolumes entries). Updated each
    // frame in Tick(). UPLOAD-heap StructuredBuffer<VolumeGPUDesc>.
    RHI::GPUBuffer  m_volumeBuffer;
    void*           m_volumeBufferMapped = nullptr;
    uint64_t        m_volumeBufferSrv    = 0;

    // Per-resource-type contiguous descriptor blocks (kMaxVolumes slots each).
    // Allocated once at Init from the static GPU-visible region; the GPU
    // base handle stays stable for the lifetime of the manager. Individual
    // slot SRVs are (re-)written by AllocateOrUpdate / DestroyVolume via
    // GraphicsDX12::CreateXxxSRVAtCpu — no per-frame copy work.
    DescriptorAllocation m_probeSHTable;
    DescriptorAllocation m_depthTable;
    DescriptorAllocation m_probeDataTable;
    uint64_t             m_probeSHTableGpu   = 0;
    uint64_t             m_depthTableGpu     = 0;
    uint64_t             m_probeDataTableGpu = 0;

    // Write {real or null} SRVs for the slot's resources into the 3 tables.
    // Called from CreateAtlasesForVolume() (real) and DestroyVolume() (null).
    void WriteSlotDescriptors  (IGraphicsDevice& gfx, uint32_t slot, const VolumeResources& res);
    void WriteSlotNullDescriptors(IGraphicsDevice& gfx, uint32_t slot);
};

// Helper: depth atlas dimensions for a given probe grid. Used by the manager
// (allocation) and the relight CS / border CS dispatches. Irradiance atlas
// is gone — probe irradiance is now a per-probe SH structured buffer.
inline uint32_t DepthAtlasWidth      (uint32_t pcx, uint32_t)        { return pcx * kDepthProbeStride; }
inline uint32_t DepthAtlasHeight     (uint32_t,    uint32_t pcy_pcz) { return pcy_pcz * kDepthProbeStride; }

} // namespace DDGI
