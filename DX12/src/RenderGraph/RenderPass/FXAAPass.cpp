#include "RenderGraph/RenderPass/FXAAPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>

// Compute root signature slots — must match GraphicsDX12::CreateComputeRootSignature.
static constexpr uint32_t kCBSlot    = 0;
static constexpr uint32_t kInputSRV  = 1;  // t0 space2
static constexpr uint32_t kOutputUAV = 4;  // u0 space2

void FXAAPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::FXAA_CS, RHI::ShaderStage::CS, "FXAA.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::FXAA_CS);
    if (!cs)
    {
        LOG_ERROR("FXAAPass: FXAA_CS shader not found");
        return;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("FXAAPass: PSO creation failed");
        return;
    }

    m_cb.Create(gfx, "FXAA.CB");

    LOG_SUCCESS("FXAAPass: initialized");
}

uint64_t FXAAPass::GetResolvedSrvHandle() const
{
    if (!m_gfx || !m_output.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_output);
}

void FXAAPass::RebuildBuffers()
{
    if (!m_gfx || m_vpW == 0 || m_vpH == 0) return;

    if (m_output.IsValid())
        m_gfx->DestroyTexture(m_output);

    RHI::TextureDesc td{};
    td.width      = m_vpW;
    td.height     = m_vpH;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, m_output))
        LOG_ERROR("FXAAPass: failed to create output buffer");

    m_outputState = RHI::ResourceState::UNORDERED_ACCESS;
    m_lastVpW     = m_vpW;
    m_lastVpH     = m_vpH;

    LOG_INFO("FXAAPass: rebuilt output buffer %ux%u", m_vpW, m_vpH);
}

RHI::CommandList FXAAPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled) return cl;
    if (!m_pso.IsValid() || m_vpW == 0 || m_vpH == 0) return cl;
    if (!m_inputSrvHandle) return cl;

    if (m_vpW != m_lastVpW || m_vpH != m_lastVpH)
        RebuildBuffers();
    if (!m_output.IsValid()) return cl;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Upload CB
    m_cbData.width                  = m_vpW;
    m_cbData.height                 = m_vpH;
    m_cbData.qualitySubpix          = qualitySubpix;
    m_cbData.qualityEdgeThreshold   = qualityEdgeThreshold;
    m_cbData.qualityEdgeThresholdMin= qualityEdgeThresholdMin;
    if (auto* slot = m_cb.Current(gfx)) *slot = m_cbData;

    // Output → UAV
    if (m_outputState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_output,
            m_outputState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_outputState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // Dispatch
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kInputSRV, m_inputSrvHandle, cl);
    const uint64_t outputUav = gfx.GetTextureUAVGpuHandle(m_output);
    if (outputUav)
        gfx.SetComputeDescriptorTable(kOutputUAV, outputUav, cl);

    gfx.DispatchCompute((m_vpW + 7) / 8, (m_vpH + 7) / 8, 1, cl);

    // Output → SRV for downstream reads
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_output,
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_outputState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    return cl;
}
