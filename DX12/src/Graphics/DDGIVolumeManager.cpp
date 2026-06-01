#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"     // multi-volume table fill — DX12 helpers
#include "ECS/DDGIComponents.h"
#include "System/Log.h"

#include <cstring>
#include <cmath>

using namespace DirectX;

namespace DDGI
{

// =============================================================================
// Init / Shutdown
// =============================================================================

bool DDGIVolumeManager::Init(IGraphicsDevice& gfx)
{
    // Engine-wide volume desc buffer (kMaxVolumes entries, UPLOAD heap so the
    // CPU can rewrite it each frame without staging copies). Triple-buffered
    // ring — written every frame by Tick().
    RHI::GPUBufferDesc desc{};
    desc.size       = uint64_t(kMaxVolumes) * sizeof(VolumeGPUDesc);
    desc.stride     = sizeof(VolumeGPUDesc);
    desc.usage      = RHI::Usage::UPLOAD;
    desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    desc.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (!gfx.CreateBuffer(desc, m_volumeBuffer[i]))
        {
            LOG_ERROR("DDGIVolumeManager: volume buffer create failed (slot %u)", i);
            return false;
        }
        m_volumeBufferMapped[i] = gfx.MapBuffer(m_volumeBuffer[i]);
        m_volumeBufferSrv[i]    = gfx.GetBufferSRVGpuHandle(m_volumeBuffer[i]);
        if (m_volumeBufferMapped[i])
            std::memset(m_volumeBufferMapped[i], 0, desc.size);
    }

    // Allocate the three multi-volume descriptor tables (one block of
    // kMaxVolumes contiguous SRV slots each — for ProbeSH / Depth / ProbeData).
    // Pre-fill every slot with a null SRV so unallocated entries hold valid
    // descriptors (the shader's vi < ddgiVolumeCount loop never reads them but
    // D3D12 validation flags any uninitialised SRV slot in a bound table).
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    DescriptorHeapAllocator& alloc = dx12.GetCbvSrvUavAllocator();

    constexpr uint32_t kProbeSHStride   = 48;       // sizeof(DDGIProbeSH)
    constexpr uint32_t kProbeDataStride = sizeof(ProbeData);
    constexpr DXGI_FORMAT kDepthFormat  = DXGI_FORMAT_R16G16_FLOAT;

    auto allocAndNullFill = [&](DescriptorAllocation& outAlloc,
                                uint64_t&             outGpu,
                                auto                  fillNullFn)
    {
        outAlloc = alloc.AllocateStatic(kMaxVolumes);
        if (!outAlloc.IsValid())
        {
            LOG_ERROR("DDGIVolumeManager: descriptor table allocation failed (kMaxVolumes=%u)", kMaxVolumes);
            return false;
        }
        const UINT descSize = alloc.GetDescriptorSize();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = outAlloc.GetGpuCpuHandle();
        for (uint32_t i = 0; i < kMaxVolumes; ++i)
        {
            fillNullFn(cpu);
            cpu.ptr += descSize;
        }
        outGpu = outAlloc.GetGpuHandle().ptr;
        return true;
    };

    if (!allocAndNullFill(m_probeSHTable, m_probeSHTableGpu,
            [&](D3D12_CPU_DESCRIPTOR_HANDLE h) { dx12.CreateNullStructuredBufferSRVAtCpu(kProbeSHStride, h); }))
        return false;
    if (!allocAndNullFill(m_depthTable, m_depthTableGpu,
            [&](D3D12_CPU_DESCRIPTOR_HANDLE h) { dx12.CreateNullTexture2DSRVAtCpu(kDepthFormat, h); }))
        return false;
    if (!allocAndNullFill(m_probeDataTable, m_probeDataTableGpu,
            [&](D3D12_CPU_DESCRIPTOR_HANDLE h) { dx12.CreateNullStructuredBufferSRVAtCpu(kProbeDataStride, h); }))
        return false;

    LOG_INFO("DDGIVolumeManager: ready (max %u volumes; SH/Depth/ProbeData tables allocated)", kMaxVolumes);
    // Build marker — grep DX12Log.txt for this string to confirm the running
    // exe actually contains the latest DDGI changes. If it's absent (or shows
    // an older tag), a stale build is being run.
    LOG_INFO("DDGIVolumeManager: [stability-build 2026-05-14d] adaptive off + rotation amplitude scaled to ray count");
    return true;
}

void DDGIVolumeManager::Shutdown(IGraphicsDevice& gfx)
{
    for (uint32_t i = 0; i < kMaxVolumes; ++i)
        if (m_volumes[i].allocated)
            DestroyVolume(gfx, m_volumes[i]);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_volumeBuffer[i].IsValid())
        {
            if (m_volumeBufferMapped[i]) gfx.UnmapBuffer(m_volumeBuffer[i]);
            gfx.DestroyBuffer(m_volumeBuffer[i]);
            m_volumeBufferMapped[i] = nullptr;
            m_volumeBufferSrv[i]    = 0;
        }
    }
    if (m_probeSHTable.IsValid())   m_probeSHTable.Free();
    if (m_depthTable.IsValid())     m_depthTable.Free();
    if (m_probeDataTable.IsValid()) m_probeDataTable.Free();
    m_probeSHTableGpu = m_depthTableGpu = m_probeDataTableGpu = 0;
    m_activeCount = 0;
}

// Write the slot's three SRVs into the kMaxVolumes-element tables at offset
// `slot`. Called after CreateAtlasesForVolume succeeds. The GPU handles the
// shader sees stay constant — this is a CPU-side rewrite of the descriptor
// at a stable position.
void DDGIVolumeManager::WriteSlotDescriptors(IGraphicsDevice& gfx, uint32_t slot,
                                             const VolumeResources& res)
{
    if (slot >= kMaxVolumes) return;
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    const UINT descSize = dx12.GetCbvSrvUavAllocator().GetDescriptorSize();
    auto offsetCpu = [&](D3D12_CPU_DESCRIPTOR_HANDLE base, uint32_t s)
    {
        base.ptr += SIZE_T(s) * descSize;
        return base;
    };

    if (m_probeSHTable.IsValid())
        dx12.CreateStructuredBufferSRVAtCpu(res.probeSHBuffer,
            offsetCpu(m_probeSHTable.GetGpuCpuHandle(), slot));
    if (m_depthTable.IsValid())
        dx12.CreateTexture2DSRVAtCpu(res.depthAtlas,
            offsetCpu(m_depthTable.GetGpuCpuHandle(), slot));
    if (m_probeDataTable.IsValid())
        dx12.CreateStructuredBufferSRVAtCpu(res.probeDataBuffer,
            offsetCpu(m_probeDataTable.GetGpuCpuHandle(), slot));
}

void DDGIVolumeManager::WriteSlotNullDescriptors(IGraphicsDevice& gfx, uint32_t slot)
{
    if (slot >= kMaxVolumes) return;
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    const UINT descSize = dx12.GetCbvSrvUavAllocator().GetDescriptorSize();
    auto offsetCpu = [&](D3D12_CPU_DESCRIPTOR_HANDLE base, uint32_t s)
    {
        base.ptr += SIZE_T(s) * descSize;
        return base;
    };

    constexpr uint32_t kProbeSHStride   = 48;
    constexpr uint32_t kProbeDataStride = sizeof(ProbeData);
    constexpr DXGI_FORMAT kDepthFormat  = DXGI_FORMAT_R16G16_FLOAT;

    if (m_probeSHTable.IsValid())
        dx12.CreateNullStructuredBufferSRVAtCpu(kProbeSHStride,
            offsetCpu(m_probeSHTable.GetGpuCpuHandle(), slot));
    if (m_depthTable.IsValid())
        dx12.CreateNullTexture2DSRVAtCpu(kDepthFormat,
            offsetCpu(m_depthTable.GetGpuCpuHandle(), slot));
    if (m_probeDataTable.IsValid())
        dx12.CreateNullStructuredBufferSRVAtCpu(kProbeDataStride,
            offsetCpu(m_probeDataTable.GetGpuCpuHandle(), slot));
}

// =============================================================================
// Allocation
// =============================================================================

bool DDGIVolumeManager::CreateAtlasesForVolume(IGraphicsDevice& gfx,
                                               const DDGIVolumeComponent& v,
                                               VolumeResources& res)
{
    const uint32_t pcx = v.probeCountsX;
    const uint32_t pcy = v.probeCountsY;
    const uint32_t pcz = v.probeCountsZ;
    if (pcx == 0 || pcy == 0 || pcz == 0)
    {
        LOG_ERROR("DDGIVolumeManager: zero probe count not allowed (%u,%u,%u)", pcx, pcy, pcz);
        return false;
    }
    const uint32_t totalProbes = pcx * pcy * pcz;
    const uint32_t pcyPcz      = pcy * pcz;

    // ---- Probe SH buffer (RWStructuredBuffer<DDGIProbeSH>) ------------------
    // Replaces the old irradiance atlas. 48 B per probe — 3 float4 (RGB × L1
    // SH coefficients). Zero-init so DDGI_SH_Irradiance returns 0 until the
    // first relight pass populates the coefficients.
    {
        constexpr uint32_t kProbeSHStride = 48;
        RHI::GPUBufferDesc bd{};
        bd.size       = uint64_t(totalProbes) * kProbeSHStride;
        bd.stride     = kProbeSHStride;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        std::vector<uint8_t> zeros(bd.size, 0);
        if (!gfx.CreateBuffer(bd, res.probeSHBuffer, zeros.data()))
        {
            LOG_ERROR("DDGIVolumeManager: probe SH buffer create failed");
            return false;
        }
        res.probeSHSrv = gfx.GetBufferSRVGpuHandle(res.probeSHBuffer);
        res.probeSHUav = gfx.GetBufferUAVGpuHandle(res.probeSHBuffer);
    }

    // ---- Depth atlas (R16G16F) ---------------------------------------------
    {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
        td.format     = RHI::Format::R16G16_FLOAT;
        td.width      = DepthAtlasWidth (pcx, pcyPcz);
        td.height     = DepthAtlasHeight(pcx, pcyPcz);
        td.array_size = 1;
        td.mip_levels = 1;
        td.usage      = RHI::Usage::DEFAULT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        // R16G16F = 4 bytes per pixel.
        std::vector<uint8_t> zeros(uint64_t(td.width) * td.height * 4, 0);
        RHI::SubresourceData init{ zeros.data(), td.width * 4u, 0u };
        if (!gfx.CreateTexture(td, res.depthAtlas, &init))
        {
            LOG_ERROR("DDGIVolumeManager: depth atlas create failed (%ux%u)",
                      td.width, td.height);
            return false;
        }
        res.depthAtlasSrv = gfx.GetTextureSRVGpuHandle(res.depthAtlas);
        res.depthAtlasUav = gfx.GetTextureUAVGpuHandle(res.depthAtlas);
    }

    // ---- Probe data buffer (RWStructuredBuffer<ProbeData>) -----------------
    // Zero-init so every probe starts as ACTIVE (state encoding chosen so
    // 0 == ACTIVE) with offset (0,0,0). Without explicit init, garbage memory
    // could land state in the INACTIVE region and the sampler would skip
    // those probes, leaving black holes in the irradiance.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = uint64_t(totalProbes) * sizeof(ProbeData);
        bd.stride     = sizeof(ProbeData);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        std::vector<uint8_t> zeros(bd.size, 0);
        if (!gfx.CreateBuffer(bd, res.probeDataBuffer, zeros.data()))
        {
            LOG_ERROR("DDGIVolumeManager: probe data buffer create failed");
            return false;
        }
        res.probeDataSrv = gfx.GetBufferSRVGpuHandle(res.probeDataBuffer);
        res.probeDataUav = gfx.GetBufferUAVGpuHandle(res.probeDataBuffer);
    }

    // ---- Ray data buffer (RWStructuredBuffer<float4>) ----------------------
    // One float4 per (probe, ray): rgb = traced radiance, a = hit distance.
    {
        const uint64_t rayCount = uint64_t(totalProbes) * v.raysPerProbe;
        RHI::GPUBufferDesc bd{};
        bd.size       = rayCount * sizeof(float) * 4;
        bd.stride     = sizeof(float) * 4;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (!gfx.CreateBuffer(bd, res.rayDataBuffer))
        {
            LOG_ERROR("DDGIVolumeManager: ray data buffer create failed");
            return false;
        }
        res.rayDataSrv = gfx.GetBufferSRVGpuHandle(res.rayDataBuffer);
        res.rayDataUav = gfx.GetBufferUAVGpuHandle(res.rayDataBuffer);
    }

    // ---- Ray count buffer (RWStructuredBuffer<uint>) -----------------------
    // Per-probe bucket count consumed by the trace's ExecuteIndirect path and
    // by the relight's ray-loop. Initialised to MAX so the first frame uses
    // the full ray budget; later frames adapt down based on variance.
    {
        const uint32_t maxBuckets = v.raysPerProbe / DDGI_RAY_BUCKET_COUNT;
        RHI::GPUBufferDesc bd{};
        bd.size       = uint64_t(totalProbes) * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        std::vector<uint32_t> initial(totalProbes, maxBuckets);
        if (!gfx.CreateBuffer(bd, res.rayCountBuffer, initial.data()))
        {
            LOG_ERROR("DDGIVolumeManager: ray count buffer create failed");
            return false;
        }
        res.rayCountUav = gfx.GetBufferUAVGpuHandle(res.rayCountBuffer);
    }

    // ---- Ray allocation buffer ---------------------------------------------
    // [0]    = total rays (running atomic during allocate).
    // [1..]  = packed ray descriptors  (probeIdx | (rayIdx << 20)).
    // Stays in UAV state for the entire frame — read by trace CS as UAV.
    {
        const uint64_t maxRays = uint64_t(totalProbes) * v.raysPerProbe;
        const uint64_t entries = 1 + maxRays;
        RHI::GPUBufferDesc bd{};
        bd.size       = entries * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (!gfx.CreateBuffer(bd, res.rayAllocBuffer))
        {
            LOG_ERROR("DDGIVolumeManager: ray alloc buffer create failed");
            return false;
        }
        res.rayAllocUav = gfx.GetBufferUAVGpuHandle(res.rayAllocBuffer);
    }

    // ---- Dispatch args buffer ----------------------------------------------
    // 16 B (4 uints, last one unused) — the finalize CS writes [0..2] here
    // and the trace's ExecuteIndirect reads them after a UAV→INDIRECT_ARGUMENT
    // transition. D3D12 has no dedicated bind flag for indirect-args use; any
    // UAV-capable buffer can be transitioned into INDIRECT_ARGUMENT state.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = 16;
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (!gfx.CreateBuffer(bd, res.dispatchArgsBuffer))
        {
            LOG_ERROR("DDGIVolumeManager: dispatch args buffer create failed");
            return false;
        }
        res.dispatchArgsUav = gfx.GetBufferUAVGpuHandle(res.dispatchArgsBuffer);
    }

    // ---- Variance buffer (RWStructuredBuffer<DDGIVarianceData>) ------------
    // 48 bytes per (probe, irradiance-tile texel). Holds the multi-scale mean
    // estimator state (mean / shortMean / variance / vbbr / inconsistency).
    // Must be zero-initialised — DDGIRelight.cs lazily seeds it on the first
    // overwrite-mode pass, but a non-zero initial state can land in NaN /
    // negative-variance territory.
    {
        constexpr uint32_t kVarianceStride = 48;
        const uint64_t entries = uint64_t(totalProbes) * kIrradianceProbeSize * kIrradianceProbeSize;
        RHI::GPUBufferDesc bd{};
        bd.size       = entries * kVarianceStride;
        bd.stride     = kVarianceStride;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        std::vector<uint8_t> zeros(bd.size, 0);
        if (!gfx.CreateBuffer(bd, res.varianceBuffer, zeros.data()))
        {
            LOG_ERROR("DDGIVolumeManager: variance buffer create failed");
            return false;
        }
        res.varianceUav = gfx.GetBufferUAVGpuHandle(res.varianceBuffer);
    }

    // ---- Volume CB ring (UPLOAD heap, persistently mapped) ------------------
    // Triple-buffered — Tick() rewrites randomRotation + frameIndex every
    // frame; a single CB would race the GPU's in-flight DDGIPass read of the
    // prior frame's data under 3-frame CPU pipelining.
    for (uint32_t f = 0; f < kFrameCount; ++f)
    {
        // 256-byte aligned per CBV. The struct is < 256 bytes — pad to that.
        RHI::GPUBufferDesc bd{};
        bd.size       = 256;
        bd.stride     = 0;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (!gfx.CreateBuffer(bd, res.volumeCB[f]))
        {
            LOG_ERROR("DDGIVolumeManager: volume CB create failed (slot %u)", f);
            return false;
        }
        res.cbvMapped[f]     = gfx.MapBuffer(res.volumeCB[f]);
        res.cbvGpuAddress[f] = 0; // backend doesn't expose CB GPU VA directly here;
                                  // the manager will look it up via SetComputeRootCBV
                                  // which the device handles. Keep field for future
                                  // direct-bind use cases.
        if (res.cbvMapped[f]) std::memset(res.cbvMapped[f], 0, 256);
    }

    res.probeCountsX = pcx;
    res.probeCountsY = pcy;
    res.probeCountsZ = pcz;
    res.raysPerProbe = v.raysPerProbe;
    res.allocated    = true;
    res.atlasState   = AtlasState::UAV; // CreateTexture with layout=UAV
    return true;
}

void DDGIVolumeManager::DestroyVolume(IGraphicsDevice& gfx, VolumeResources& res)
{
    if (!res.allocated) return;
    for (uint32_t f = 0; f < kFrameCount; ++f)
    {
        if (res.cbvMapped[f])      { gfx.UnmapBuffer(res.volumeCB[f]); res.cbvMapped[f] = nullptr; }
        if (res.volumeCB[f].IsValid()) gfx.DestroyBuffer(res.volumeCB[f]);
    }
    if (res.varianceBuffer.IsValid())      gfx.DestroyBuffer (res.varianceBuffer);
    if (res.dispatchArgsBuffer.IsValid())  gfx.DestroyBuffer (res.dispatchArgsBuffer);
    if (res.rayAllocBuffer.IsValid())      gfx.DestroyBuffer (res.rayAllocBuffer);
    if (res.rayCountBuffer.IsValid())      gfx.DestroyBuffer (res.rayCountBuffer);
    if (res.rayDataBuffer.IsValid())       gfx.DestroyBuffer (res.rayDataBuffer);
    if (res.probeDataBuffer.IsValid())  gfx.DestroyBuffer (res.probeDataBuffer);
    if (res.depthAtlas.IsValid())       gfx.DestroyTexture(res.depthAtlas);
    if (res.probeSHBuffer.IsValid())    gfx.DestroyBuffer (res.probeSHBuffer);
    res = {};
}

bool DDGIVolumeManager::AllocateOrUpdate(IGraphicsDevice& gfx,
                                         const DDGIVolumeComponent& volume,
                                         DDGIVolumeRuntimeComponent& runtime)
{
    // Re-use existing slot when component already has one and probe layout
    // hasn't changed; otherwise pick a new slot.
    auto sameLayout = [&](const VolumeResources& r) {
        return r.allocated
            && r.probeCountsX == volume.probeCountsX
            && r.probeCountsY == volume.probeCountsY
            && r.probeCountsZ == volume.probeCountsZ
            && r.raysPerProbe == volume.raysPerProbe;
    };

    if (runtime.volumeSlot < kMaxVolumes && sameLayout(m_volumes[runtime.volumeSlot]))
    {
        // Sync runtime cache for resize-detection on next frame.
        runtime.probeCountX  = volume.probeCountsX;
        runtime.probeCountY  = volume.probeCountsY;
        runtime.probeCountZ  = volume.probeCountsZ;
        runtime.raysPerProbe = volume.raysPerProbe;
        return true;
    }

    // Free old slot if size changed.
    if (runtime.volumeSlot < kMaxVolumes && m_volumes[runtime.volumeSlot].allocated)
        DestroyVolume(gfx, m_volumes[runtime.volumeSlot]);

    // Find first free slot if needed.
    if (runtime.volumeSlot >= kMaxVolumes)
    {
        for (uint32_t i = 0; i < kMaxVolumes; ++i)
            if (!m_volumes[i].allocated) { runtime.volumeSlot = i; break; }
    }
    if (runtime.volumeSlot >= kMaxVolumes)
    {
        LOG_ERROR("DDGIVolumeManager: no free volume slot (max=%u)", kMaxVolumes);
        return false;
    }

    if (!CreateAtlasesForVolume(gfx, volume, m_volumes[runtime.volumeSlot]))
        return false;

    // Publish this slot's SRVs into the multi-volume tables. The shader sees a
    // stable GPU handle into each table; only the descriptor at offset
    // `volumeSlot` changes when this slot's resources rotate.
    WriteSlotDescriptors(gfx, runtime.volumeSlot, m_volumes[runtime.volumeSlot]);

    runtime.probeCountX  = volume.probeCountsX;
    runtime.probeCountY  = volume.probeCountsY;
    runtime.probeCountZ  = volume.probeCountsZ;
    runtime.raysPerProbe = volume.raysPerProbe;
    runtime.frameCounter = 0;
    // Force the next 3 relight passes to overwrite the atlas (no lerp). This
    // is the only reliable way to flush whatever GPU memory the texture was
    // born into — initialData uploads work on most drivers but we don't
    // want to rely on that for every R11G11B10F texture format combo.
    runtime.firstFramesRemaining = 3;

    m_activeCount = 0;
    for (uint32_t i = 0; i < kMaxVolumes; ++i)
        if (m_volumes[i].allocated) m_activeCount++;

    LOG_INFO("DDGIVolumeManager: volume slot %u allocated (%ux%ux%u probes, %u rays)",
             runtime.volumeSlot, volume.probeCountsX, volume.probeCountsY,
             volume.probeCountsZ, volume.raysPerProbe);
    return true;
}

void DDGIVolumeManager::Free(IGraphicsDevice& gfx, uint32_t slot)
{
    if (slot >= kMaxVolumes || !m_volumes[slot].allocated) return;
    WriteSlotNullDescriptors(gfx, slot);   // unbind before resources die
    DestroyVolume(gfx, m_volumes[slot]);
    m_activeCount = 0;
    for (uint32_t i = 0; i < kMaxVolumes; ++i)
        if (m_volumes[i].allocated) m_activeCount++;
}

void DDGIVolumeManager::FreeUnclaimedSlots(IGraphicsDevice& gfx,
                                           const uint32_t* claimedSlots,
                                           uint32_t        claimedCount)
{
    bool claimed[kMaxVolumes] = {};
    for (uint32_t i = 0; i < claimedCount; ++i)
    {
        uint32_t s = claimedSlots[i];
        if (s < kMaxVolumes) claimed[s] = true;
    }
    for (uint32_t s = 0; s < kMaxVolumes; ++s)
    {
        if (m_volumes[s].allocated && !claimed[s])
        {
            LOG_INFO("DDGIVolumeManager: freeing orphaned slot %u", s);
            WriteSlotNullDescriptors(gfx, s);
            DestroyVolume(gfx, m_volumes[s]);
        }
    }
    m_activeCount = 0;
    for (uint32_t i = 0; i < kMaxVolumes; ++i)
        if (m_volumes[i].allocated) m_activeCount++;
}

// =============================================================================
// Per-frame Tick — pack VolumeGPUDesc array
// =============================================================================

namespace
{
// Per-frame rotation seed for the probe ray set — van der Corput on
// (frame, slot), amplitude scaled to the Halton point spacing.
//
// IMPORTANT — this is INTENTIONALLY uncorrelated frame-to-frame; do NOT
// "smooth" it into a slow drift. The rotation jitter the relight EMA sees is
// HIGH frequency (a fresh orientation every frame), which the EMA filters
// well. A 2026-05-14 attempt to make this a smooth slow drift made the
// per-frame ray set evolve at LOW frequency — exactly what the EMA passes
// through — and the whole-scene "breathing" got visibly WORSE. Reverted.
//
// AMPLITUDE — the jitter only needs to fill the gaps BETWEEN the fixed Halton
// directions. The angular gap radius for N directions on a sphere is
// ≈ sqrt(π / N). The old fixed 0.3 rad (≈17°) was tuned for ~64 rays; at 256
// rays the gaps are only ≈6.4°, so 17° was ~3× oversized — every frame it
// swept many rays across radiance discontinuities (sky↔wall edges), injecting
// per-frame noise that bought no extra coverage and forced a high hysteresis.
// Scaling kAmp to sqrt(π / raysPerProbe) keeps just-enough dither: more rays →
// smaller gaps → smaller jitter → less per-frame noise → lower stable
// hysteresis. Still uncorrelated (high-frequency) — only the amplitude shrank.
void GenerateRandomRotation(uint32_t frame, uint32_t slot, uint32_t raysPerProbe,
                            float out[9])
{
    auto vdc = [](uint32_t i, uint32_t base) {
        float r = 0.0f, f = 1.0f / float(base);
        while (i > 0) { r += f * float(i % base); i /= base; f /= float(base); }
        return r;
    };
    const uint32_t k  = frame * 73u + slot * 19u + 1u;
    // kAmp ≈ Halton gap radius for raysPerProbe directions, clamped to a sane
    // band so a degenerate ray count can't make it 0 or absurdly large.
    float kAmp = std::sqrt(3.14159265f / float(raysPerProbe < 1u ? 1u : raysPerProbe));
    if (kAmp < 0.05f) kAmp = 0.05f;
    if (kAmp > 0.35f) kAmp = 0.35f;
    const float    ax = vdc(k, 2) * kAmp;
    const float    ay = vdc(k, 3) * kAmp;
    const float    az = vdc(k, 5) * kAmp;

    const float cx = std::cos(ax), sx = std::sin(ax);
    const float cy = std::cos(ay), sy = std::sin(ay);
    const float cz = std::cos(az), sz = std::sin(az);

    // R = Rz * Ry * Rx (intrinsic XYZ — composed left-to-right).
    const float r00 =  cy * cz;
    const float r01 =  sx * sy * cz - cx * sz;
    const float r02 =  cx * sy * cz + sx * sz;
    const float r10 =  cy * sz;
    const float r11 =  sx * sy * sz + cx * cz;
    const float r12 =  cx * sy * sz - sx * cz;
    const float r20 = -sy;
    const float r21 =  sx * cy;
    const float r22 =  cx * cy;
    out[0]=r00; out[1]=r01; out[2]=r02;
    out[3]=r10; out[4]=r11; out[5]=r12;
    out[6]=r20; out[7]=r21; out[8]=r22;
}
} // namespace

void DDGIVolumeManager::Tick(IGraphicsDevice& gfx,
                             const DDGIVolumeComponent* const* volumes,
                             DDGIVolumeRuntimeComponent* const* runtimes,
                             uint32_t volumeCount,
                             uint32_t lightCount)
{
    const uint32_t frameSlot = gfx.GetFrameIndex();
    if (frameSlot >= kFrameCount || !m_volumeBufferMapped[frameSlot]) return;
    auto* gpuDesc = static_cast<VolumeGPUDesc*>(m_volumeBufferMapped[frameSlot]);
    std::memset(gpuDesc, 0, kMaxVolumes * sizeof(VolumeGPUDesc));

    // Slot-indexed packing: each entry in the volume buffer is at
    // gpuDesc[r.volumeSlot] (NOT gpuDesc[written]). This keeps the shader's
    // index space aligned with the per-resource descriptor tables, which are
    // also slot-indexed. Inactive slots stay zero-init (flags & 1u == 0 →
    // shader skip). The shader iterates 0..DDGI_MAX_VOLUMES and gates each
    // iteration on (flags & 1u).
    uint32_t written = 0;
    for (uint32_t i = 0; i < volumeCount; ++i)
    {
        if (!volumes[i] || !runtimes[i]) continue;
        const DDGIVolumeComponent& v = *volumes[i];
        DDGIVolumeRuntimeComponent& r = *runtimes[i];
        if (r.volumeSlot >= kMaxVolumes || !m_volumes[r.volumeSlot].allocated) continue;

        VolumeResources& res = m_volumes[r.volumeSlot];

        // Advance frame + roll a fresh rotation. Amplitude scales to the
        // ray count (more rays → tighter Halton gaps → smaller dither needed).
        r.frameCounter++;
        GenerateRandomRotation(r.frameCounter, r.volumeSlot, v.raysPerProbe,
                               r.randomRotation);

        VolumeGPUDesc& d = gpuDesc[r.volumeSlot];
        d.origin       = v.origin;
        d.extent       = v.extent;
        const XMFLOAT3 cellCount{ float(v.probeCountsX > 1 ? v.probeCountsX - 1 : 1),
                                  float(v.probeCountsY > 1 ? v.probeCountsY - 1 : 1),
                                  float(v.probeCountsZ > 1 ? v.probeCountsZ - 1 : 1) };
        d.probeSpacing = { (2.0f * v.extent.x) / cellCount.x,
                           (2.0f * v.extent.y) / cellCount.y,
                           (2.0f * v.extent.z) / cellCount.z };
        d.probeCountsX = v.probeCountsX;
        d.probeCountsY = v.probeCountsY;
        d.probeCountsZ = v.probeCountsZ;
        d.raysPerProbe = v.raysPerProbe;
        d.hysteresis   = v.hysteresis;
        d.normalBias   = v.normalBias;
        d.viewBias     = v.viewBias;
        d.boundaryFadeRatio = v.boundaryFadeRatio;

        uint32_t flags = 0;
        flags |= 1u; // bit 0 — enabled
        if (v.enableRelocation)     flags |= 2u;  // bit 1
        if (v.enableClassification) flags |= 4u;  // bit 2
        if (r.firstFramesRemaining > 0)
        {
            flags |= 8u;                          // bit 3 — overwrite atlas
            r.firstFramesRemaining--;
        }
        d.flags = flags;

        d.randomRotation0 = { r.randomRotation[0], r.randomRotation[1], r.randomRotation[2], 0.0f };
        d.randomRotation1 = { r.randomRotation[3], r.randomRotation[4], r.randomRotation[5], 0.0f };
        d.randomRotation2 = { r.randomRotation[6], r.randomRotation[7], r.randomRotation[8], 0.0f };

        d.diffuseTint  = v.diffuseTint;
        d.diffuseScale = v.diffuseScale;
        d.lightCount   = lightCount;
        d.frameIndex   = r.frameCounter;

        // Per-volume CB mirrors the same data — the DDGI passes bind only the
        // CB (not the engine-wide buffer) to keep their root sigs simple.
        // Write to the current frame's ring slot.
        if (res.cbvMapped[frameSlot])
            std::memcpy(res.cbvMapped[frameSlot], &d, sizeof(VolumeGPUDesc));

        written++;
    }
    (void)gfx;
}

// =============================================================================
// Accessors
// =============================================================================

uint64_t DDGIVolumeManager::GetProbeSHSrv(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].probeSHSrv : 0; }
uint64_t DDGIVolumeManager::GetDepthAtlasSrv(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].depthAtlasSrv : 0; }
uint64_t DDGIVolumeManager::GetProbeDataSrv(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].probeDataSrv : 0; }
uint64_t DDGIVolumeManager::GetRayDataSrv(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].rayDataSrv : 0; }
uint64_t DDGIVolumeManager::GetProbeSHUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].probeSHUav : 0; }
uint64_t DDGIVolumeManager::GetDepthAtlasUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].depthAtlasUav : 0; }
uint64_t DDGIVolumeManager::GetRayDataUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].rayDataUav : 0; }
uint64_t DDGIVolumeManager::GetProbeDataUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].probeDataUav : 0; }
uint64_t DDGIVolumeManager::GetVarianceUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].varianceUav : 0; }
uint64_t DDGIVolumeManager::GetRayCountUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].rayCountUav : 0; }
uint64_t DDGIVolumeManager::GetRayAllocUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].rayAllocUav : 0; }
uint64_t DDGIVolumeManager::GetDispatchArgsUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].dispatchArgsUav : 0; }
const RHI::GPUBuffer* DDGIVolumeManager::GetDispatchArgsBuffer(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].dispatchArgsBuffer : nullptr; }
const RHI::GPUBuffer* DDGIVolumeManager::GetProbeSHBuffer(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].probeSHBuffer : nullptr; }
const RHI::GPUBuffer* DDGIVolumeManager::GetProbeDataBuffer(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].probeDataBuffer : nullptr; }
uint64_t DDGIVolumeManager::GetVolumeCBVGpuAddress(IGraphicsDevice& gfx, uint32_t s) const
{
    if (s >= kMaxVolumes) return 0;
    const uint32_t f = gfx.GetFrameIndex();
    return (f < kFrameCount) ? m_volumes[s].cbvGpuAddress[f] : 0;
}
const RHI::GPUBuffer* DDGIVolumeManager::GetVolumeCB(IGraphicsDevice& gfx, uint32_t s) const
{
    if (s >= kMaxVolumes || !m_volumes[s].allocated) return nullptr;
    const uint32_t f = gfx.GetFrameIndex();
    return (f < kFrameCount) ? &m_volumes[s].volumeCB[f] : nullptr;
}

void DDGIVolumeManager::TransitionVolumeAtlases(IGraphicsDevice& gfx,
                                                RHI::CommandList cmd,
                                                uint32_t slot, AtlasState newState,
                                                bool computeQueueState)
{
    if (slot >= kMaxVolumes || !m_volumes[slot].allocated) return;
    VolumeResources& r = m_volumes[slot];

    // Resolve the SRV state mask we want for this transition target. The full
    // SHADER_RESOURCE (PIXEL | NON_PIXEL) is only legal on graphics queues; on
    // compute we restrict to NON_PIXEL_SHADER_RESOURCE (the only SRV state
    // valid on D3D12_COMMAND_LIST_TYPE_COMPUTE).
    const RHI::ResourceState srvTargetState = computeQueueState
        ? RHI::ResourceState::SHADER_RESOURCE_COMPUTE
        : RHI::ResourceState::SHADER_RESOURCE;

    // Skip when the recorded high-level state already matches AND the queue
    // restriction is consistent — re-issuing the same transition would have
    // StateBefore == StateAfter, which D3D12 flags.
    if (r.atlasState == newState
        && (newState == AtlasState::UAV || r.atlasInComputeOnlyState == computeQueueState))
        return;

    // Resolve the StateBefore mask we currently believe the atlas is in. If
    // the previous transition was a compute-queue SRV (NON_PIXEL only) and we
    // are now transitioning on the graphics queue, the caller MUST have used
    // PromoteAtlasesForGraphicsQueue first — we treat that as a precondition
    // and emit the regular SHADER_RESOURCE state transition here.
    const RHI::ResourceState before = (r.atlasState == AtlasState::UAV)
        ? RHI::ResourceState::UNORDERED_ACCESS
        : (r.atlasInComputeOnlyState
            ? RHI::ResourceState::SHADER_RESOURCE_COMPUTE
            : RHI::ResourceState::SHADER_RESOURCE);
    const RHI::ResourceState after = (newState == AtlasState::UAV)
        ? RHI::ResourceState::UNORDERED_ACCESS
        : srvTargetState;

    // Only the depth atlas is a texture now; the SH probe buffer stays UAV
    // throughout (UAV reads are valid in shaders, no transition needed).
    RHI::GPUBarrier b = RHI::GPUBarrier::Image(&r.depthAtlas, before, after);
    gfx.PushBarrier(b, cmd);
    r.atlasState = newState;
    r.atlasInComputeOnlyState = (newState == AtlasState::SRV) ? computeQueueState : false;
}

void DDGIVolumeManager::PromoteAtlasesForGraphicsQueue(IGraphicsDevice& gfx,
                                                       RHI::CommandList graphicsCmd)
{
    // Per-active-slot transition: NON_PIXEL_SHADER_RESOURCE → SHADER_RESOURCE
    // (PIXEL | NON_PIXEL). Caller MUST have already enforced the cross-queue
    // fence so the prior compute-queue writes are visible to this transition.
    //
    // Three resources promote per slot:
    //   1. depth atlas (Texture2D R16G16F) — texture barrier, gated on atlas
    //      state.
    //   2. probe SH buffer (StructuredBuffer<DDGIProbeSH>) — buffer barrier;
    //      DDGIPass leaves it in SH-read state at end-of-frame, which on a
    //      compute queue means NON_PIXEL only.
    //   3. probeData buffer — same compute-only SH-read state at end of
    //      Execute; LightingPass samples it via the relocation offset.
    for (uint32_t s = 0; s < kMaxVolumes; ++s)
    {
        VolumeResources& r = m_volumes[s];
        if (!r.allocated) continue;

        if (r.atlasInComputeOnlyState)
        {
            if (r.atlasState == AtlasState::SRV)
            {
                RHI::GPUBarrier b = RHI::GPUBarrier::Image(&r.depthAtlas,
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                    RHI::ResourceState::SHADER_RESOURCE);
                gfx.PushBarrier(b, graphicsCmd);
            }
            r.atlasInComputeOnlyState = false;
        }

        if (r.buffersInComputeOnlyState)
        {
            // SH probe buffer — DDGIPass restored it to its "shader-read"
            // state (kSHReadState) at the end of Execute. On the compute
            // queue that's NON_PIXEL only; promote to NON_PIXEL | PIXEL
            // for the lighting PS.
            RHI::GPUBarrier shB = RHI::GPUBarrier::Buffer(&r.probeSHBuffer,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                RHI::ResourceState::SHADER_RESOURCE);
            gfx.PushBarrier(shB, graphicsCmd);

            // ProbeData buffer — same state cycle as the SH buffer.
            RHI::GPUBarrier pdB = RHI::GPUBarrier::Buffer(&r.probeDataBuffer,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                RHI::ResourceState::SHADER_RESOURCE);
            gfx.PushBarrier(pdB, graphicsCmd);

            r.buffersInComputeOnlyState = false;
        }
    }
}

void DDGIVolumeManager::DemoteForComputeQueue(IGraphicsDevice& gfx,
                                              RHI::CommandList graphicsCmd)
{
    // Symmetric inverse of PromoteAtlasesForGraphicsQueue — see header doc.
    // Gates on the per-resource compute-only-state flags so re-entry is a
    // no-op (e.g. first frame after Allocate the depth atlas is at UAV state,
    // not SR, so the atlas barrier is skipped while the buffer barriers fire).
    for (uint32_t s = 0; s < kMaxVolumes; ++s)
    {
        VolumeResources& r = m_volumes[s];
        if (!r.allocated) continue;

        // Depth atlas: only demote when currently in SRV state with the full
        // PSR|NPSR mask. UAV state doesn't have PSR, so nothing to do.
        if (!r.atlasInComputeOnlyState && r.atlasState == AtlasState::SRV)
        {
            RHI::GPUBarrier b = RHI::GPUBarrier::Image(&r.depthAtlas,
                RHI::ResourceState::SHADER_RESOURCE,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            gfx.PushBarrier(b, graphicsCmd);
            r.atlasInComputeOnlyState = true;
        }

        if (!r.buffersInComputeOnlyState)
        {
            RHI::GPUBarrier shB = RHI::GPUBarrier::Buffer(&r.probeSHBuffer,
                RHI::ResourceState::SHADER_RESOURCE,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            gfx.PushBarrier(shB, graphicsCmd);

            RHI::GPUBarrier pdB = RHI::GPUBarrier::Buffer(&r.probeDataBuffer,
                RHI::ResourceState::SHADER_RESOURCE,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            gfx.PushBarrier(pdB, graphicsCmd);

            r.buffersInComputeOnlyState = true;
        }
    }
}

uint32_t DDGIVolumeManager::GetProbeCount(uint32_t s) const
{
    if (s >= kMaxVolumes || !m_volumes[s].allocated) return 0;
    const VolumeResources& r = m_volumes[s];
    return r.probeCountsX * r.probeCountsY * r.probeCountsZ;
}
uint32_t DDGIVolumeManager::GetRaysPerProbe(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].raysPerProbe : 0; }
uint32_t DDGIVolumeManager::GetDepthAtlasWidth(uint32_t s) const
{ return s < kMaxVolumes && m_volumes[s].allocated
       ? DepthAtlasWidth(m_volumes[s].probeCountsX,
                         m_volumes[s].probeCountsY * m_volumes[s].probeCountsZ)
       : 0; }
uint32_t DDGIVolumeManager::GetDepthAtlasHeight(uint32_t s) const
{ return s < kMaxVolumes && m_volumes[s].allocated
       ? DepthAtlasHeight(m_volumes[s].probeCountsX,
                          m_volumes[s].probeCountsY * m_volumes[s].probeCountsZ)
       : 0; }

// =============================================================================
// Per-frame accessors
// =============================================================================

uint64_t DDGIVolumeManager::GetVolumeBufferSrv(IGraphicsDevice& gfx) const
{
    const uint32_t s = gfx.GetFrameIndex();
    return (s < kFrameCount) ? m_volumeBufferSrv[s] : 0;
}

} // namespace DDGI
