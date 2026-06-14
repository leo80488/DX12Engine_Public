#include "RenderGraph/RenderPass/UnderwaterPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

// Compute root signature slots (shared with every compute pass, space2).
static constexpr uint32_t kCBSlot = 0;  // b0 space2
static constexpr uint32_t kSRV0   = 1;  // t0 space2
static constexpr uint32_t kUAV0   = 4;  // u0 space2

void UnderwaterPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Underwater_CS, RHI::ShaderStage::CS,
                         "Underwater.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::Underwater_CS);
    if (!cs)
    {
        LOG_ERROR("UnderwaterPass: Underwater_CS shader not found");
        return;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("UnderwaterPass: PSO creation failed");
        return;
    }

    m_cb.Create(gfx, "Underwater.CB");
    LOG_SUCCESS("UnderwaterPass: initialized");
}

void UnderwaterPass::SetViewportSize(uint32_t w, uint32_t h)
{
    if (w != m_vpW || h != m_vpH)
    {
        m_vpW = w;
        m_vpH = h;
        m_texDirty = true;
    }
}

void UnderwaterPass::RebuildTextures()
{
    if (!m_gfx || m_vpW == 0 || m_vpH == 0) return;

    if (m_output.IsValid()) m_gfx->DestroyTexture(m_output);

    RHI::TextureDesc td{};
    td.width      = m_vpW;
    td.height     = m_vpH;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    if (!m_gfx->CreateTexture(td, m_output))
        LOG_ERROR("UnderwaterPass: failed to create output texture (%ux%u)", m_vpW, m_vpH);

    m_outputState = RHI::ResourceState::UNORDERED_ACCESS;
    m_texDirty    = false;
}

uint64_t UnderwaterPass::GetOutputSrvHandle() const
{
    if (!m_gfx || !m_output.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_output);
}

void UnderwaterPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled || !m_pso.IsValid()) return;
    if (!m_inputSrv || m_vpW == 0 || m_vpH == 0) return;

    if (m_texDirty) RebuildTextures();
    if (!m_output.IsValid()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    if (auto* slot = m_cb.Current(gfx))
    {
        UnderwaterCB c{};
        c.width      = m_vpW;
        c.height     = m_vpH;
        c.time       = m_time;
        c.strength   = strength;
        c.scale      = scale;
        c.speed      = speed;
        c.tintAmount = tintAmount;
        c.tint[0]    = tint.x;
        c.tint[1]    = tint.y;
        c.tint[2]    = tint.z;
        *slot = c;
    }

    if (m_outputState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_output, m_outputState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_outputState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRV0, m_inputSrv, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_output), cl);
    gfx.DispatchCompute((m_vpW + 7) / 8, (m_vpH + 7) / 8, 1, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_output, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_outputState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
}
