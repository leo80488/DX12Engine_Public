#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>

static constexpr uint32_t kCBSlot      = 0;
static constexpr uint32_t kSRV0        = 1;   // HDR SRV  (histogram build only)
static constexpr uint32_t kUAV0        = 4;   // histogram UAV
static constexpr uint32_t kUAV1        = 5;   // exposure UAV

static constexpr uint32_t kHistBins = 256;

// ---------------------------------------------------------------------------
void AutoExposurePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::HistogramBuild_CS,   RHI::ShaderStage::CS,
                         "AutoExposure.cs.hlsl", "CSHistogramBuild");
    m_shaderLib.Register(ShaderID::HistogramAverage_CS, RHI::ShaderStage::CS,
                         "AutoExposure.cs.hlsl", "CSHistogramAverage");

    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::HistogramBuild_CS);
        if (!cs) { LOG_ERROR("AutoExposurePass: HistogramBuild_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_buildPSO))
            LOG_ERROR("AutoExposurePass: build PSO failed");
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::HistogramAverage_CS);
        if (!cs) { LOG_ERROR("AutoExposurePass: HistogramAverage_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_averagePSO))
            LOG_ERROR("AutoExposurePass: average PSO failed");
    }

    // Histogram: 256 × uint (UAV only)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kHistBins * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS;
        
        uint32_t zeros[kHistBins] = {0};
        if (!gfx.CreateBuffer(bd, m_histogramBuffer, zeros))
            LOG_ERROR("AutoExposurePass: histogram buffer failed");
    }

    // Exposure: 1 × float (UAV + SRV for ToneMap to read)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(float);
        bd.stride     = sizeof(float);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        
        float initialExposure = 1.0f;
        if (!gfx.CreateBuffer(bd, m_exposureBuffer, &initialExposure))
            LOG_ERROR("AutoExposurePass: exposure buffer failed");
    }

    // Per-dispatch CB
    m_cb.Create(gfx, "AutoExposure.CB");

    // Staging for manual-exposure upload when the pass is disabled.
    // Ringed across kFrameCount slots — the CopyBuffer the disabled-path
    // queues each frame reads from one of these, so the next frame's CPU
    // write must not land on the same buffer that the GPU is still copying.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(float);
        bd.stride     = sizeof(float);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_manualStaging[i]))
                m_manualStagingMapped[i] = gfx.MapBuffer(m_manualStaging[i]);
        }
    }

    LOG_SUCCESS("AutoExposurePass: initialized");
}

// ---------------------------------------------------------------------------
RHI::CommandList AutoExposurePass::Execute(RHI::CommandList cl)
{
    if (!m_buildPSO.IsValid() || !m_averagePSO.IsValid()) return cl;
    if (m_vpW == 0 || m_vpH == 0 || m_hdrSrvHandle == 0) return cl;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // ---- Disabled path: upload the manual-exposure scalar to the same
    // buffer ToneMap reads, then early-return. Skips histogram dispatches
    // entirely (zero GPU cost beyond a 4-byte copy).
    if (!m_enabled)
    {
        const uint32_t frameSlot = gfx.GetFrameIndex();
        if (m_manualStagingMapped[frameSlot])
            std::memcpy(m_manualStagingMapped[frameSlot], &m_manualExposure, sizeof(float));

        if (m_exposureState != RHI::ResourceState::COPY_DST)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(
                &m_exposureBuffer, m_exposureState, RHI::ResourceState::COPY_DST), cl);
            m_exposureState = RHI::ResourceState::COPY_DST;
        }
        gfx.CopyBuffer(m_manualStaging[frameSlot], m_exposureBuffer, sizeof(float), cl);

        gfx.PushBarrier(RHI::GPUBarrier::Buffer(
            &m_exposureBuffer, RHI::ResourceState::COPY_DST,
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_exposureState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        return cl;
    }

    if (auto* slot = m_cb.Current(gfx))
    {
        AutoExposurePass::AutoExposureCB cb{};
        cb.width           = m_vpW;
        cb.height          = m_vpH;
        cb.minLogLuma      = m_minLogLuma;
        cb.invLogLumaRange = 1.0f / (m_maxLogLuma - m_minLogLuma);
        cb.adaptationRate  = m_adaptationRate;
        cb.lowPercent      = m_lowPercent;
        cb.highPercent     = m_highPercent;
        cb.minExposure     = m_minExposure;
        cb.maxExposure     = m_maxExposure;
        cb.evBias          = m_evBias;
        cb.keyValue        = m_keyValue;
        *slot = cb;
    }

    // UAV barrier to ensure histogram writes from last frame are visible
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_histogramBuffer), cl);

    if (m_exposureState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_exposureBuffer, m_exposureState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_exposureState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(gfx);

    // --- Histogram Build ---
    gfx.BindComputePipelineState(m_buildPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, cbBuf, cl);
    gfx.SetComputeDescriptorTable(kSRV0,  m_hdrSrvHandle, cl);
    gfx.SetComputeDescriptorTable(kUAV0,  gfx.GetBufferUAVGpuHandle(m_histogramBuffer), cl);
    gfx.DispatchCompute((m_vpW + 15) / 16, (m_vpH + 15) / 16, 1, cl);

    // UAV barrier before average
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_histogramBuffer), cl);
    // --- Histogram Average ---
    gfx.BindComputePipelineState(m_averagePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, cbBuf, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetBufferUAVGpuHandle(m_histogramBuffer), cl);
    gfx.SetComputeDescriptorTable(kUAV1, gfx.GetBufferUAVGpuHandle(m_exposureBuffer),  cl);
    gfx.DispatchCompute(1, 1, 1, cl);

    // UAV barrier on exposure so ToneMap can read it safely
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_exposureBuffer), cl);

    // Transition exposure buffer to SRV for ToneMapPass
    if (m_exposureState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_exposureBuffer, m_exposureState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_exposureState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }

    return cl;
}
