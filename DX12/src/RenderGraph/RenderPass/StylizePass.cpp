#include "RenderGraph/RenderPass/StylizePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

static constexpr uint32_t kCBSlot = 0;  // b0 space2
static constexpr uint32_t kSRV0   = 1;  // t0 space2
static constexpr uint32_t kUAV0   = 4;  // u0 space2

void StylizePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Stylize_CS, RHI::ShaderStage::CS,
                         "Stylize.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::Stylize_CS);
    if (!cs)
    {
        LOG_ERROR("StylizePass: Stylize_CS shader not found");
        return;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("StylizePass: PSO creation failed");
        return;
    }

    m_cb.Create(gfx, "Stylize.CB");
    LOG_SUCCESS("StylizePass: initialized");
}

void StylizePass::SetViewportSize(uint32_t w, uint32_t h)
{
    if (w != m_vpW || h != m_vpH)
    {
        m_vpW = w;
        m_vpH = h;
        m_texDirty = true;
    }
}

void StylizePass::RebuildTextures()
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
        LOG_ERROR("StylizePass: failed to create output texture (%ux%u)", m_vpW, m_vpH);

    m_outputState = RHI::ResourceState::UNORDERED_ACCESS;
    m_texDirty    = false;
}

uint64_t StylizePass::GetOutputSrvHandle() const
{
    if (!m_gfx || !m_output.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_output);
}

void StylizePass::Execute(RHI::CommandList cl)
{
    if (!m_enabled || !m_pso.IsValid()) return;
    if (!m_inputSrv || m_vpW == 0 || m_vpH == 0) return;

    if (m_texDirty) RebuildTextures();
    if (!m_output.IsValid()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    if (auto* slot = m_cb.Current(gfx))
    {
        StylizeCB c{};
        c.width               = m_vpW;
        c.height              = m_vpH;
        c.kuwaharaOn          = kuwaharaOn ? 1u : 0u;
        c.posterizeOn         = posterizeOn ? 1u : 0u;
        c.halftoneOn          = halftoneOn ? 1u : 0u;
        c.ditherOn            = ditherOn ? 1u : 0u;
        c.crosshatchOn        = crosshatchOn ? 1u : 0u;
        c.kuwaharaRadius      = kuwaharaRadius;
        c.posterizeLevels     = static_cast<float>(posterizeLevels);
        c.halftoneCell        = halftoneCell;
        c.halftoneAngle       = halftoneAngle;
        c.ditherLevels        = static_cast<float>(ditherLevels);
        c.crosshatchDensity   = crosshatchDensity;
        c.crosshatchThickness = crosshatchThickness;
        c.pixelateOn          = pixelateOn ? 1u : 0u;
        c.pixelSize           = pixelSize;
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
