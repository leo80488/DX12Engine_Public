#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>

// Compute root signature slot assignments (shared across compute passes).
static constexpr uint32_t kCBSlot = 0;
static constexpr uint32_t kSRV0   = 1;  // t0 space2 — depth
static constexpr uint32_t kSRV1   = 2;  // t1 space2 — cloud transmittance
static constexpr uint32_t kUAV0   = 4;  // u0 space2 — output flare

// ---------------------------------------------------------------------------
void LensFlarePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::LensFlare_CS, RHI::ShaderStage::CS,
                         "LensFlare.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::LensFlare_CS);
    if (!cs) { LOG_ERROR("LensFlarePass: LensFlare_CS not found"); return; }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("LensFlarePass: PSO failed"); return; }

    m_cb.Create(gfx, "LensFlare.CB");

    LOG_SUCCESS("LensFlarePass: initialized");
}

// ---------------------------------------------------------------------------
void LensFlarePass::DestroyTexture()
{
    if (m_gfx && m_texture.IsValid())
        m_gfx->DestroyTexture(m_texture);
}

void LensFlarePass::RebuildTexture()
{
    if (!m_gfx) return;
    DestroyTexture();

    m_texW = (m_vpW + 1) / 2;
    m_texH = (m_vpH + 1) / 2;
    if (m_texW < 1) m_texW = 1;
    if (m_texH < 1) m_texH = 1;

    RHI::TextureDesc td{};
    td.width      = m_texW;
    td.height     = m_texH;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debug_name = "LensFlarePass.Output";

    if (!m_gfx->CreateTexture(td, m_texture))
        LOG_ERROR("LensFlarePass: failed to create output texture");

    m_textureState = RHI::ResourceState::UNORDERED_ACCESS;
    m_lastVpW = m_vpW;
    m_lastVpH = m_vpH;
    LOG_INFO("LensFlarePass: output texture %ux%u", m_texW, m_texH);
}

// ---------------------------------------------------------------------------
uint64_t LensFlarePass::GetSrvHandle() const
{
    if (!m_gfx || !m_texture.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_texture);
}

// ---------------------------------------------------------------------------
RHI::CommandList LensFlarePass::Execute(RHI::CommandList cl)
{
    if (!m_pso.IsValid())   return cl;
    if (m_vpW == 0 || m_vpH == 0) return cl;

    if (m_vpW != m_lastVpW || m_vpH != m_lastVpH)
        RebuildTexture();
    if (!m_texture.IsValid()) return cl;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Texture left as SHADER_RESOURCE_COMPUTE last frame (consumed by ToneMap)
    // → flip back to UAV before writing.
    if (m_textureState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_texture, m_textureState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_textureState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // Upload CB.
    if (auto* slot = m_cb.Current(gfx))
    {
        LensFlarePass::LensFlareCB cb{};
        cb.dstWidth        = m_texW;
        cb.dstHeight       = m_texH;
        cb.srcDepthWidth   = m_depthW ? m_depthW : m_vpW;
        cb.srcDepthHeight  = m_depthH ? m_depthH : m_vpH;
        cb.enabled         = (m_enabled && m_depthSrv != 0) ? 1u : 0u;
        cb.intensity       = m_intensity;
        cb.sunBehind       = m_sunBehind ? 1.0f : 0.0f;
        cb.chromaticOffset = m_chromaticOffset;
        cb.sunUV[0]        = m_sunUV.x;
        cb.sunUV[1]        = m_sunUV.y;
        cb.haloWidth       = m_haloWidth;
        cb.streakLength    = m_streakLength;
        cb.sunColor[0]     = m_sunColor.x;
        cb.sunColor[1]     = m_sunColor.y;
        cb.sunColor[2]     = m_sunColor.z;
        cb.ghostDispersal  = m_ghostDispersal;
        cb.ghostCount      = (m_ghostCount > 8u) ? 8u : m_ghostCount;
        cb.streakWidth     = 0.014f;
        cb.occlusionRadius = 0.006f;
        cb.cloudValid      = (m_cloudSrv != 0) ? 1u : 0u;
        *slot = cb;
    }

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    if (m_depthSrv) gfx.SetComputeDescriptorTable(kSRV0, m_depthSrv, cl);
    // The shader branches on cloudValid; bind depth as a harmless dummy when
    // clouds didn't render so the statically-declared t1 table is never unset.
    if (m_cloudSrv || m_depthSrv)
        gfx.SetComputeDescriptorTable(kSRV1, m_cloudSrv ? m_cloudSrv : m_depthSrv, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_texture), cl);

    gfx.DispatchCompute((m_texW + 7) / 8, (m_texH + 7) / 8, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_texture), cl);

    // Hand off to ToneMap as SRV.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_texture, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_textureState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    return cl;
}
