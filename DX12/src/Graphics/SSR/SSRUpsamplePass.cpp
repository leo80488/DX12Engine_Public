#include "Graphics/SSR/SSRUpsamplePass.h"
#include "Graphics/SSR/SSRRootSigSlots.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>

// SSRUpsamplePass — variance-driven bilateral blur AND half→full upsample
// boundary (Pass 5). Reads half-res temporal color + variance via UV-based
// SampleLevel, writes a full-res reflection texture.

using namespace SSR::RootSig;

static_assert(sizeof(SSRUpsamplePass::SSRUpsampleCB) <= 256, "SSRUpsampleCB > 256");

void SSRUpsamplePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRUpsample_CS, RHI::ShaderStage::CS,
                         "SSRUpsample.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRUpsample_CS);
    if (!cs) { LOG_ERROR("SSRUpsamplePass: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRUpsamplePass: PSO failed"); return; }

    m_cb.Create(gfx, "SSRUpsample.CB");

    LOG_SUCCESS("SSRUpsamplePass: initialised");
}

void SSRUpsamplePass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRUpsample_CS);
    if (!cs) { LOG_ERROR("SSRUpsamplePass::ReloadShaders: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("SSRUpsamplePass::ReloadShaders: PSO rebuild failed");
}

void SSRUpsamplePass::EnsureTexture(uint32_t w, uint32_t h)
{
    if (!m_gfx || w == 0 || h == 0) return;
    if (w == m_w && h == m_h && m_colorTex.IsValid()) return;

    if (m_colorTex.IsValid()) m_gfx->DestroyTexture(m_colorTex);
    m_w = w; m_h = h;

    RHI::TextureDesc td{};
    td.width = w; td.height = h;
    td.format = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage = RHI::Usage::DEFAULT;
    td.layout = RHI::ResourceState::SHADER_RESOURCE;
    if (!m_gfx->CreateTexture(td, m_colorTex))
        LOG_ERROR("SSRUpsamplePass: output create failed");
    LOG_INFO("SSRUpsamplePass: buffer %ux%u ready", w, h);
}

uint64_t SSRUpsamplePass::GetColorSrv() const
{
    if (!m_gfx || !m_colorTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_colorTex);
}

void SSRUpsamplePass::Execute(RHI::CommandList cl,
                              uint32_t w, uint32_t h,
                              uint64_t temporalSrv, uint64_t varianceSrv,
                              uint64_t depthSrv, uint64_t normalSrv, uint64_t surfaceSrv)
{
    if (!m_pso.IsValid() || !m_cb.IsValid()) return;
    if (w != m_w || h != m_h) return;
    if (!temporalSrv || !varianceSrv) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);

    SSRUpsamplePass::SSRUpsampleCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    cb.screenW = w; cb.screenH = h;
    cb.invScreenW = 1.0f / float(w);
    cb.invScreenH = 1.0f / float(h);
    cb.nearZ = m_cam.nearZ;
    cb.farZ  = m_cam.farZ;
    if (auto* slot = m_cb.Current(dx12)) *slot = cb;

    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_colorTex, RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(dx12), 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, temporalSrv, cl);  // t0
    dx12.SetComputeDescriptorTable(kSRV_T1, varianceSrv, cl);  // t1
    dx12.SetComputeDescriptorTable(kSRV_T2, depthSrv,    cl);  // t2
    dx12.SetComputeDescriptorTable(kSRV_T3, normalSrv,   cl);  // t3
    dx12.SetComputeDescriptorTable(kSRV_T4, surfaceSrv,  cl);  // t4
    dx12.SetComputeDescriptorTable(kUAV_U0,
        m_gfx->GetTextureUAVGpuHandle(m_colorTex), cl);

    const uint32_t gx = (w + 7) / 8;
    const uint32_t gy = (h + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);

    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_colorTex, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}
