#include "RenderGraph/RenderPass/ClusterPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>

// GPU-side Light struct — must match GPULight in cluster_common.hlsli (64 bytes).
// _pad[0] is now interpreted by the shader as `asuint` to recover a spot light's
// shadow-atlas slice index (0xFFFFFFFF means "no shadow map — skip sampling").
struct GPULightGPU
{
    float    position[3];
    float    radius;
    float    color[3];
    float    intensity;
    float    direction[3];
    float    spotAngle;
    uint32_t type;
    uint32_t shadowSliceIdx;       // 0xFFFFFFFF = no shadow
    uint32_t _pad[2];
};
static_assert(sizeof(GPULightGPU) == 64, "GPULightGPU must be 64 bytes");

// Compute root sig slots (must match GraphicsDX12::CreateComputeRootSignature)
static constexpr uint32_t kCBSlot0   = 0;  // b0 space2 — ClusterCB
static constexpr uint32_t kSRV0      = 1;  // t0 space2 — cluster AABBs (read)
static constexpr uint32_t kSRV1      = 2;  // t1 space2 — lights (read)
static constexpr uint32_t kSRV2      = 3;  // t2 space2 — (unused here)
static constexpr uint32_t kUAV0      = 4;  // u0 space2 — cluster AABBs (write) / light index list
static constexpr uint32_t kUAV1      = 5;  // u1 space2 — light grid
// Note: ViewCB at b1 space2 needs a second CBV binding.
// g_GlobalCounter at u2 space2 — there is no root param for u2 in the compute sig.
// We'll pack the counter into the light grid buffer instead.

// ---------------------------------------------------------------------------
void ClusterPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::ClusterBuild_CS, RHI::ShaderStage::CS,
                         "ClusterBuild.cs.hlsl", "CSBuildClusters");
    m_shaderLib.Register(ShaderID::ClusterCull_CS, RHI::ShaderStage::CS,
                         "ClusterCull.cs.hlsl", "CSCullLights");
    m_shaderLib.Register(ShaderID::ClusterCullProbes_CS, RHI::ShaderStage::CS,
                         "ClusterCullProbes.cs.hlsl");

    // Build PSOs
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::ClusterBuild_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_buildPSO))
            LOG_ERROR("ClusterPass: ClusterBuild PSO creation failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::ClusterCull_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_cullPSO))
            LOG_ERROR("ClusterPass: ClusterCull PSO creation failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::ClusterCullProbes_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_probeCullPSO))
            LOG_ERROR("ClusterPass: ClusterCullProbes PSO creation failed");
    }

    // ---- Create GPU buffers ------------------------------------------------

    // Light buffer (UPLOAD, CPU writes each frame) — triple-buffered ring so
    // the CPU's frame N+1 write can't stomp the GPU's frame N read.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxLights) * sizeof(GPULightGPU);
        bd.stride     = sizeof(GPULightGPU);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_lightBuffer[i]))
            {
                m_lightMapped[i] = gfx.MapBuffer(m_lightBuffer[i]);
                m_lightsSRV[i]   = gfx.GetBufferSRVGpuHandle(m_lightBuffer[i]);
            }
        }
    }

    // Cluster AABB buffer (DEFAULT, compute writes)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kClusterCount) * 32; // ClusterAABB = 32 bytes
        bd.stride     = 32;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_clusterAABBBuffer))
        {
            m_clusterAABBSRV = gfx.GetBufferSRVGpuHandle(m_clusterAABBBuffer);
            m_clusterAABBUAV = gfx.GetBufferUAVGpuHandle(m_clusterAABBBuffer);
        }
    }

    // Light index list (DEFAULT, compute writes, lighting reads)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxLightIndices) * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_lightIndexBuffer))
        {
            m_lightIndexSRV = gfx.GetBufferSRVGpuHandle(m_lightIndexBuffer);
            m_lightIndexUAV = gfx.GetBufferUAVGpuHandle(m_lightIndexBuffer);
        }
    }

    // Light grid (DEFAULT, compute writes, lighting reads)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kClusterCount) * 8; // LightGridEntry = 2 × uint = 8 bytes
        bd.stride     = 8;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_lightGridBuffer))
        {
            m_lightGridSRV = gfx.GetBufferSRVGpuHandle(m_lightGridBuffer);
            m_lightGridUAV = gfx.GetBufferUAVGpuHandle(m_lightGridBuffer);
        }
    }

    // Probe index list — same shape as the light index list but smaller cap.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxProbeIndices) * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_probeIndexBuffer))
        {
            m_probeIndexSRV = gfx.GetBufferSRVGpuHandle(m_probeIndexBuffer);
            m_probeIndexUAV = gfx.GetBufferUAVGpuHandle(m_probeIndexBuffer);
        }
    }

    // Probe grid (per-cluster offset + count, identical layout to LightGridEntry).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kClusterCount) * 8; // ProbeGridEntry = 2 × uint
        bd.stride     = 8;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_probeGridBuffer))
        {
            m_probeGridSRV = gfx.GetBufferSRVGpuHandle(m_probeGridBuffer);
            m_probeGridUAV = gfx.GetBufferUAVGpuHandle(m_probeGridBuffer);
        }
    }

    // Atomic counter (single uint)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS;
        if (gfx.CreateBuffer(bd, m_counterBuffer))
            m_counterUAV = gfx.GetBufferUAVGpuHandle(m_counterBuffer);
    }

    // Constant buffer (UPLOAD) — triple-buffered FrameCB.
    if (!m_clusterCB.Create(gfx, "ClusterPass.CB"))
        LOG_ERROR("ClusterPass: cluster CB create failed");

    // Counter reset buffer (4 bytes UPLOAD, contains a single 0 for copy-to-counter)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(uint32_t);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::NONE;
        if (gfx.CreateBuffer(bd, m_counterResetBuf))
        {
            m_counterResetMapped = gfx.MapBuffer(m_counterResetBuf);
            if (m_counterResetMapped)
                *static_cast<uint32_t*>(m_counterResetMapped) = 0;
        }
    }

    LOG_SUCCESS("ClusterPass: initialized (%u clusters, max %u lights, %u index budget)",
                kClusterCount, kMaxLights, kMaxLightIndices);
}

// ---------------------------------------------------------------------------
void ClusterPass::SetLights(const std::vector<ResolvedLight>& lights)
{
    m_lightCount = std::min(static_cast<uint32_t>(lights.size()), kMaxLights);
    if (!m_gfx || m_lightCount == 0) return;
    const uint32_t frameSlot = m_gfx->GetFrameIndex();
    if (frameSlot >= kFrameCount || !m_lightMapped[frameSlot]) return;

    auto* dst = static_cast<GPULightGPU*>(m_lightMapped[frameSlot]);
    for (uint32_t i = 0; i < m_lightCount; ++i)
    {
        const ResolvedLight& src = lights[i];
        dst[i].position[0]  = src.position.x;
        dst[i].position[1]  = src.position.y;
        dst[i].position[2]  = src.position.z;
        dst[i].radius       = src.radius;
        dst[i].color[0]     = src.color.x;
        dst[i].color[1]     = src.color.y;
        dst[i].color[2]     = src.color.z;
        dst[i].intensity    = src.intensity;
        dst[i].direction[0] = src.direction.x;
        dst[i].direction[1] = src.direction.y;
        dst[i].direction[2] = src.direction.z;
        dst[i].spotAngle    = src.spotAngle;
        dst[i].type           = static_cast<uint32_t>(src.type);
        dst[i].shadowSliceIdx = src.shadowSliceIdx;
        dst[i]._pad[0]        = 0u;
        dst[i]._pad[1]        = 0u;
    }
}

// ---------------------------------------------------------------------------
void ClusterPass::SetCamera(const DirectX::XMFLOAT4X4& invProj,
                             const DirectX::XMFLOAT4X4& viewMatrix,
                             float nearZ, float farZ,
                             uint32_t screenW, uint32_t screenH)
{
    std::memcpy(m_cbData.invProj, &invProj, 64);
    m_cbData.nearZ      = nearZ;
    m_cbData.farZ       = farZ;
    m_cbData.screenW    = screenW;
    m_cbData.screenH    = screenH;
    m_cbData.lightCount = m_lightCount;
    m_cbData.probeCount = m_probeCount;
    std::memcpy(m_cbData.viewMatrix, &viewMatrix, 64);

    if (screenW != m_lastScreenW || screenH != m_lastScreenH)
    {
        m_needsRebuild = true;
        m_lastScreenW  = screenW;
        m_lastScreenH  = screenH;
    }
}

// ---------------------------------------------------------------------------
void ClusterPass::Execute(RHI::CommandList cl)
{
    if (!m_gfx || !m_buildPSO.IsValid() || !m_cullPSO.IsValid()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // probeCount lives on the pass and may be set after SetCamera — refresh
    // it here so the dispatched ClusterCullProbes sees the latest value.
    m_cbData.probeCount = m_probeCount;

    // Upload CB into this frame's slot.
    if (auto* p = m_clusterCB.Current(gfx))
        *p = m_cbData;

    const uint32_t frameSlot = gfx.GetFrameIndex();
    const RHI::GPUBuffer& cbBuf = m_clusterCB.CurrentBuffer(gfx);

    // ---- Pass 1: Build cluster AABBs (only on resize) ----
    if (m_needsRebuild)
    {
        gfx.BindComputePipelineState(m_buildPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot0, cbBuf, 0, cl);
        gfx.SetComputeDescriptorTable(kUAV0, m_clusterAABBUAV, cl);

        gfx.DispatchCompute(kTileX, kTileY, kSlices, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_clusterAABBBuffer), cl);
        m_needsRebuild = false;
    }

    // ---- Pass 2: Cull lights into clusters ----
    if (m_lightCount > 0)
    {
        // Fixed-layout culling: each cluster writes to its own pre-allocated slice.
        // No atomic counter needed — each thread is independent.
        gfx.BindComputePipelineState(m_cullPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot0, cbBuf, 0, cl);

        gfx.SetComputeDescriptorTable(kSRV0, m_clusterAABBSRV, cl);
        gfx.SetComputeDescriptorTable(kSRV1, m_lightsSRV[frameSlot], cl);
        gfx.SetComputeDescriptorTable(kUAV0, m_lightIndexUAV, cl);
        gfx.SetComputeDescriptorTable(kUAV1, m_lightGridUAV, cl);

        gfx.DispatchCompute((kClusterCount + 63) / 64, 1, 1, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_lightIndexBuffer), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_lightGridBuffer), cl);
    }

    // ---- Pass 3: Cull reflection probes into clusters ----
    // Skipped when no probes exist OR the probe SRV hasn't been wired yet —
    // the lighting shader's `probeGrid[idx].count == 0` short-circuit covers
    // the resulting all-zero grid contents.
    if (m_probeCullPSO.IsValid() && m_probeCount > 0 && m_probeBufferSRV != 0)
    {
        gfx.BindComputePipelineState(m_probeCullPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot0, cbBuf, 0, cl);

        // Reuse t0 (cluster AABBs) and rebind t2 to the probe buffer; UAVs
        // u0 / u1 swap from light grid to probe grid.
        gfx.SetComputeDescriptorTable(kSRV0, m_clusterAABBSRV, cl);
        gfx.SetComputeDescriptorTable(kSRV2, m_probeBufferSRV, cl);
        gfx.SetComputeDescriptorTable(kUAV0, m_probeIndexUAV, cl);
        gfx.SetComputeDescriptorTable(kUAV1, m_probeGridUAV, cl);

        gfx.DispatchCompute((kClusterCount + 63) / 64, 1, 1, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_probeIndexBuffer), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_probeGridBuffer), cl);
    }
}

// ---------------------------------------------------------------------------
// Per-frame SRV accessor — returns the slot matching gfx.GetFrameIndex() so
// LightingPass / DDGIPass / etc. always sample the buffer the CPU just wrote.
uint64_t ClusterPass::GetLightsSRVHandle() const
{
    if (!m_gfx) return 0;
    const uint32_t s = m_gfx->GetFrameIndex();
    return (s < kFrameCount) ? m_lightsSRV[s] : 0;
}
