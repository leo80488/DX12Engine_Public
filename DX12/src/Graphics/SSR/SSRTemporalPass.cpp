#include "Graphics/SSR/SSRTemporalPass.h"
#include "Graphics/SSR/SSRRootSigSlots.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>

// SSRTemporalPass — dual-reprojection history accumulation (Pass 4).
// Velocity-based reproj + reflection-hit reproj compete; the candidate
// whose history luminance better matches the current pixel wins. History
// is then clamped via YCoCg AABB to suppress ghosting.

using namespace SSR::RootSig;

static_assert(sizeof(SSRTemporalPass::SSRTemporalCB) <= 256, "SSRTemporalCB > 256");

void SSRTemporalPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRTemporal_CS, RHI::ShaderStage::CS,
                         "SSRTemporal.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRTemporal_CS);
    if (!cs) { LOG_ERROR("SSRTemporalPass: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRTemporalPass: PSO failed"); return; }

    m_cb.Create(gfx, "SSRTemporal.CB");

    LOG_SUCCESS("SSRTemporalPass: initialised");
}

void SSRTemporalPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRTemporal_CS);
    if (!cs) { LOG_ERROR("SSRTemporalPass::ReloadShaders: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("SSRTemporalPass::ReloadShaders: PSO rebuild failed");
    // Wipe history so the first post-reload frame doesn't blend a stale
    // accumulator from before the shader change.
    m_resetHistory = true;
}

void SSRTemporalPass::EnsureTextures(uint32_t renderW, uint32_t renderH)
{
    if (!m_gfx || renderW == 0 || renderH == 0) return;

    // Phase 7: full-res. See SSRTracePass for rationale.
    const uint32_t traceW = renderW;
    const uint32_t traceH = renderH;

    if (renderW == m_renderW && renderH == m_renderH &&
        traceW == m_w && traceH == m_h &&
        m_color[0].IsValid() && m_color[1].IsValid() &&
        m_variance[0].IsValid() && m_variance[1].IsValid() &&
        m_depthHistory[0].IsValid() && m_depthHistory[1].IsValid())
        return;

    for (auto& t : m_color)        if (t.IsValid()) m_gfx->DestroyTexture(t);
    for (auto& t : m_variance)     if (t.IsValid()) m_gfx->DestroyTexture(t);
    for (auto& t : m_depthHistory) if (t.IsValid()) m_gfx->DestroyTexture(t);

    m_renderW = renderW; m_renderH = renderH;
    m_w = traceW;        m_h = traceH;
    m_writeIdx = 0;
    m_resetHistory = true;

    RHI::TextureDesc rgba{};
    rgba.width = traceW; rgba.height = traceH;
    rgba.format = RHI::Format::R16G16B16A16_FLOAT;
    rgba.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    rgba.usage = RHI::Usage::DEFAULT;
    rgba.layout = RHI::ResourceState::SHADER_RESOURCE;

    RHI::TextureDesc r16 = rgba;
    r16.format = RHI::Format::R16_FLOAT;

    for (int i = 0; i < 2; ++i)
    {
        if (!m_gfx->CreateTexture(rgba, m_color[i]))
            LOG_ERROR("SSRTemporalPass: color[%d] create failed", i);
        if (!m_gfx->CreateTexture(r16, m_variance[i]))
            LOG_ERROR("SSRTemporalPass: variance[%d] create failed", i);
    }

    // R32_FLOAT to match the NDC depth precision the shader expects.
    RHI::TextureDesc depth = r16;
    depth.format = RHI::Format::R32_FLOAT;
    for (int i = 0; i < 2; ++i)
    {
        if (!m_gfx->CreateTexture(depth, m_depthHistory[i]))
            LOG_ERROR("SSRTemporalPass: depthHistory[%d] create failed", i);
    }

    LOG_INFO("SSRTemporalPass: temporal buffers %ux%u (full render res) ready",
             traceW, traceH);
}

uint64_t SSRTemporalPass::GetColorSrv() const
{
    if (!m_gfx || !m_color[m_writeIdx].IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_color[m_writeIdx]);
}

uint64_t SSRTemporalPass::GetVarianceSrv() const
{
    if (!m_gfx || !m_variance[m_writeIdx].IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_variance[m_writeIdx]);
}

const RHI::Texture* SSRTemporalPass::GetColorTexture() const
{
    return &m_color[m_writeIdx];
}
const RHI::Texture* SSRTemporalPass::GetVarianceTexture() const
{
    return &m_variance[m_writeIdx];
}

void SSRTemporalPass::Execute(RHI::CommandList cl,
                              uint32_t traceW, uint32_t traceH,
                              uint64_t colorCurrentSrv,
                              uint64_t varianceCurrentSrv,
                              uint64_t reprojDepthSrv,
                              uint64_t velocitySrv,
                              uint64_t depthSrv)
{
    if (!m_pso.IsValid() || !m_cb.IsValid()) return;
    if (traceW != m_w || traceH != m_h) return;
    if (!colorCurrentSrv) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    const uint32_t readIdx = 1u - m_writeIdx;

    SSRTemporalPass::SSRTemporalCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.prevViewProj)));
    cb.traceW = traceW; cb.traceH = traceH;
    cb.invTraceW = 1.0f / float(traceW);
    cb.invTraceH = 1.0f / float(traceH);
    cb.resetHistory = m_resetHistory ? 1u : 0u;
    cb.nearZ = m_cam.nearZ;
    cb.farZ  = m_cam.farZ;
    cb.renderW = m_renderW;
    cb.renderH = m_renderH;
    cb.frameIndex = m_frameIndex;
    if (auto* slot = m_cb.Current(dx12)) *slot = cb;

    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_color[m_writeIdx], RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_variance[m_writeIdx], RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_depthHistory[m_writeIdx], RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(dx12), 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, colorCurrentSrv,    cl);    // t0
    dx12.SetComputeDescriptorTable(kSRV_T1,
        m_gfx->GetTextureSRVGpuHandle(m_color[readIdx]),    cl);        // t1
    dx12.SetComputeDescriptorTable(kSRV_T2, varianceCurrentSrv, cl);    // t2
    dx12.SetComputeDescriptorTable(kSRV_T3,
        m_gfx->GetTextureSRVGpuHandle(m_variance[readIdx]), cl);        // t3
    dx12.SetComputeDescriptorTable(kSRV_T4, reprojDepthSrv,     cl);    // t4
    if (velocitySrv)
        dx12.SetComputeDescriptorTable(kSRV_T5, velocitySrv,    cl);    // t5
    if (depthSrv)
        dx12.SetComputeDescriptorTable(kSRV_T6, depthSrv,       cl);    // t6
    dx12.SetComputeDescriptorTable(kSRV_T7,
        m_gfx->GetTextureSRVGpuHandle(m_depthHistory[readIdx]), cl);    // t7

    dx12.SetComputeDescriptorTable(kUAV_U0,
        m_gfx->GetTextureUAVGpuHandle(m_color[m_writeIdx]), cl);
    dx12.SetComputeDescriptorTable(kUAV_U1,
        m_gfx->GetTextureUAVGpuHandle(m_variance[m_writeIdx]), cl);
    dx12.SetComputeDescriptorTable(kUAV_U2,
        m_gfx->GetTextureUAVGpuHandle(m_depthHistory[m_writeIdx]), cl);

    const uint32_t gx = (traceW + 7) / 8;
    const uint32_t gy = (traceH + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);

    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_color[m_writeIdx], RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_variance[m_writeIdx], RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_depthHistory[m_writeIdx], RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);

    m_writeIdx     = readIdx;
    m_resetHistory = false;
}
