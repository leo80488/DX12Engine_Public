#include "RenderGraph/RenderPass/SSRPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>

// ===========================================================================
// Shared compute root-sig slot layout (space2).
//   param [0] = CBV b0
//   param [1] = SRV t0   [2] = SRV t1   [3] = SRV t2   [7] = SRV t3
//   param [8] = SRV t4   [6] = SRV t5   [12] = SRV t6
//   param [4] = UAV u0   [5] = UAV u1
// See GraphicsDX12::CreateComputeRootSignature for the full map.
// ===========================================================================

namespace
{
    constexpr uint32_t kCB      = 0;
    constexpr uint32_t kSRV_T0  = 1;
    constexpr uint32_t kSRV_T1  = 2;
    constexpr uint32_t kSRV_T2  = 3;
    constexpr uint32_t kSRV_T3  = 7;
    constexpr uint32_t kSRV_T4  = 8;
    constexpr uint32_t kSRV_T5  = 6;
    constexpr uint32_t kSRV_T6  = 12;
    constexpr uint32_t kSRV_T7  = 13;
    constexpr uint32_t kUAV_U0  = 4;
    constexpr uint32_t kUAV_U1  = 5;
    constexpr uint32_t kUAV_U2  = 14;
}

// ===========================================================================
// SSRPass — stochastic Hi-Z ray gen / trace
// ===========================================================================
namespace
{
    struct alignas(16) SSRTraceCB
    {
        float    viewProj[16];
        float    invViewProj[16];
        float    cameraPos[3];        float    nearZ;
        uint32_t screenW;             uint32_t screenH;
        uint32_t hizMipCount;         float    maxRayLength;
        float    farZ;                float    roughnessCutoff;
        uint32_t frameIndex;          uint32_t _pad1;
        // UE-aligned runtime knobs — see SSRTrace.cs.hlsl cbuffer.
        float    traceThickness;      uint32_t hizMostDetailedLvl;
        float    coneMipMax;          float    depthBiasFactor;
    };
    static_assert(sizeof(SSRTraceCB) <= 256, "SSRTraceCB > 256 bytes");
}

void SSRPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRTrace_CS, RHI::ShaderStage::CS,
                         "SSRTrace.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRTrace_CS);
    if (!cs) { LOG_ERROR("SSRPass: SSRTrace_CS not found"); return; }
    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRPass: PSO creation failed"); return; }

    RHI::GPUBufferDesc bd{};
    bd.size       = 512;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    LOG_SUCCESS("SSRPass: initialised (stochastic Hi-Z trace)");
}

void SSRPass::EnsureTexture(uint32_t w, uint32_t h)
{
    if (!m_gfx || w == 0 || h == 0) return;
    if (w == m_w && h == m_h &&
        m_resultTex.IsValid() &&
        m_rayDirPDFTex.IsValid() &&
        m_rayLengthTex.IsValid()) return;

    if (m_resultTex.IsValid())    m_gfx->DestroyTexture(m_resultTex);
    if (m_rayDirPDFTex.IsValid()) m_gfx->DestroyTexture(m_rayDirPDFTex);
    if (m_rayLengthTex.IsValid()) m_gfx->DestroyTexture(m_rayLengthTex);
    m_w = w; m_h = h;

    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;

    if (!m_gfx->CreateTexture(td, m_resultTex))
    { LOG_ERROR("SSRPass: hit buffer create failed (%ux%u)", w, h); return; }

    if (!m_gfx->CreateTexture(td, m_rayDirPDFTex))
    { LOG_ERROR("SSRPass: rayDirPDF create failed"); return; }

    RHI::TextureDesc td2 = td;
    td2.format = RHI::Format::R16_FLOAT;
    if (!m_gfx->CreateTexture(td2, m_rayLengthTex))
    { LOG_ERROR("SSRPass: rayLength create failed"); return; }

    m_resultState    = RHI::ResourceState::SHADER_RESOURCE;
    m_rayDirPDFState = RHI::ResourceState::SHADER_RESOURCE;
    m_rayLengthState = RHI::ResourceState::SHADER_RESOURCE;
    LOG_INFO("SSRPass: trace outputs %ux%u ready", w, h);
}

uint64_t SSRPass::GetResultSrv() const
{
    if (!m_gfx || !m_resultTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_resultTex);
}

uint64_t SSRPass::GetRayDirPDFSrv() const
{
    if (!m_gfx || !m_rayDirPDFTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_rayDirPDFTex);
}

uint64_t SSRPass::GetRayLengthSrv() const
{
    if (!m_gfx || !m_rayLengthTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_rayLengthTex);
}

void SSRPass::Execute(RHI::CommandList cl,
                      uint64_t normalSrv,
                      uint64_t surfaceSrv,
                      uint64_t depthSrv,
                      uint64_t depthHierSrv,
                      uint64_t hdrPyramidSrv,
                      uint64_t velocitySrv)
{
    if (!m_pso.IsValid() || !m_resultTex.IsValid() || !m_cbMapped) return;
    if (!normalSrv || !surfaceSrv || !depthSrv || !depthHierSrv) return;
    if (!hdrPyramidSrv) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);

    SSRTraceCB cb{};
    using namespace DirectX;
    auto storeT = [](float out[16], const XMFLOAT4X4& m) {
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(out),
            XMMatrixTranspose(XMLoadFloat4x4(&m)));
    };
    storeT(cb.viewProj,    m_cam.viewProj);
    storeT(cb.invViewProj, m_cam.invViewProj);
    cb.cameraPos[0] = m_cam.cameraPos.x;
    cb.cameraPos[1] = m_cam.cameraPos.y;
    cb.cameraPos[2] = m_cam.cameraPos.z;
    cb.nearZ           = m_cam.nearZ;
    cb.farZ            = m_cam.farZ;
    cb.screenW         = m_w;
    cb.screenH         = m_h;
    cb.hizMipCount        = m_hizMipCount ? m_hizMipCount : 1;
    cb.maxRayLength       = m_maxRayLength;
    cb.roughnessCutoff    = m_roughnessCutoff;
    cb.frameIndex         = m_frameIndex;
    cb.traceThickness     = m_traceThickness;
    cb.hizMostDetailedLvl = m_hizMostDetailed;
    cb.coneMipMax         = m_coneMipMax;
    cb.depthBiasFactor    = m_depthBiasFactor;
    std::memcpy(m_cbMapped, &cb, sizeof(cb));

    auto toUAV = [&](RHI::Texture& tex, RHI::ResourceState& state) {
        if (state != RHI::ResourceState::UNORDERED_ACCESS)
        {
            dx12.PushBarrier(RHI::GPUBarrier::Image(
                &tex, state, RHI::ResourceState::UNORDERED_ACCESS), cl);
            state = RHI::ResourceState::UNORDERED_ACCESS;
        }
    };
    toUAV(m_resultTex,    m_resultState);
    toUAV(m_rayDirPDFTex, m_rayDirPDFState);
    toUAV(m_rayLengthTex, m_rayLengthState);

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb, 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, normalSrv,     cl);  // t0
    dx12.SetComputeDescriptorTable(kSRV_T1, surfaceSrv,    cl);  // t1
    dx12.SetComputeDescriptorTable(kSRV_T2, depthSrv,      cl);  // t2
    dx12.SetComputeDescriptorTable(kSRV_T3, depthHierSrv,  cl);  // t3
    dx12.SetComputeDescriptorTable(kSRV_T4, hdrPyramidSrv, cl);  // t4
    if (velocitySrv)
        dx12.SetComputeDescriptorTable(kSRV_T7, velocitySrv, cl); // t7
    dx12.SetComputeDescriptorTable(kUAV_U0,
        m_gfx->GetTextureUAVGpuHandle(m_resultTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U1,
        m_gfx->GetTextureUAVGpuHandle(m_rayDirPDFTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U2,
        m_gfx->GetTextureUAVGpuHandle(m_rayLengthTex), cl);

    const uint32_t gx = (m_w + 7) / 8;
    const uint32_t gy = (m_h + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);
}

// ===========================================================================
// SSRResolvePass — spatial BRDF reweight (writes color + variance + reprojDepth)
// ===========================================================================
namespace
{
    struct alignas(16) SSRResolveCB
    {
        float    invViewProj[16];
        float    cameraPos[3];   float    _pad0;
        uint32_t screenW;        uint32_t screenH;
        float    invScreenW;     float    invScreenH;
        float    nearZ;          float    farZ;
        uint32_t frameIndex;     uint32_t _pad1;
        float    fireflyCap;     float    _pad3;
    };
    static_assert(sizeof(SSRResolveCB) <= 256, "SSRResolveCB > 256");
}

void SSRResolvePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRResolve_CS, RHI::ShaderStage::CS,
                         "SSRResolve.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRResolve_CS);
    if (!cs) { LOG_ERROR("SSRResolvePass: SSRResolve_CS not found"); return; }
    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRResolvePass: PSO creation failed"); return; }

    RHI::GPUBufferDesc bd{};
    bd.size       = 256;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    LOG_SUCCESS("SSRResolvePass: initialised");
}

void SSRResolvePass::EnsureTextures(uint32_t w, uint32_t h)
{
    if (!m_gfx || w == 0 || h == 0) return;
    if (w == m_w && h == m_h &&
        m_snapshotTex.IsValid() &&
        m_colorTex.IsValid() &&
        m_varianceTex.IsValid() &&
        m_reprojDepthTex.IsValid())
        return;

    if (m_snapshotTex.IsValid())    m_gfx->DestroyTexture(m_snapshotTex);
    if (m_colorTex.IsValid())       m_gfx->DestroyTexture(m_colorTex);
    if (m_varianceTex.IsValid())    m_gfx->DestroyTexture(m_varianceTex);
    if (m_reprojDepthTex.IsValid()) m_gfx->DestroyTexture(m_reprojDepthTex);

    m_w = w; m_h = h;
    m_snapshotState    = RHI::ResourceState::COPY_DST;
    m_colorState       = RHI::ResourceState::SHADER_RESOURCE;
    m_varianceState    = RHI::ResourceState::SHADER_RESOURCE;
    m_reprojDepthState = RHI::ResourceState::SHADER_RESOURCE;

    RHI::TextureDesc snap{};
    snap.width  = w; snap.height = h;
    snap.format = RHI::Format::R16G16B16A16_FLOAT;
    snap.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    snap.usage  = RHI::Usage::DEFAULT;
    snap.layout = RHI::ResourceState::COPY_DST;
    if (!m_gfx->CreateTexture(snap, m_snapshotTex))
        LOG_ERROR("SSRResolvePass: snapshot create failed");

    RHI::TextureDesc rgba{};
    rgba.width  = w; rgba.height = h;
    rgba.format = RHI::Format::R16G16B16A16_FLOAT;
    rgba.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    rgba.usage  = RHI::Usage::DEFAULT;
    rgba.layout = RHI::ResourceState::SHADER_RESOURCE;
    if (!m_gfx->CreateTexture(rgba, m_colorTex))
        LOG_ERROR("SSRResolvePass: color create failed");

    RHI::TextureDesc r16{};
    r16.width  = w; r16.height = h;
    r16.format = RHI::Format::R16_FLOAT;
    r16.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    r16.usage  = RHI::Usage::DEFAULT;
    r16.layout = RHI::ResourceState::SHADER_RESOURCE;
    if (!m_gfx->CreateTexture(r16, m_varianceTex))
        LOG_ERROR("SSRResolvePass: variance create failed");
    if (!m_gfx->CreateTexture(r16, m_reprojDepthTex))
        LOG_ERROR("SSRResolvePass: reprojDepth create failed");

    LOG_INFO("SSRResolvePass: resolve buffers %ux%u ready", w, h);
}

uint64_t SSRResolvePass::GetSnapshotSrv() const
{
    if (!m_gfx || !m_snapshotTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_snapshotTex);
}

uint64_t SSRResolvePass::GetColorSrv() const
{
    if (!m_gfx || !m_colorTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_colorTex);
}

uint64_t SSRResolvePass::GetVarianceSrv() const
{
    if (!m_gfx || !m_varianceTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_varianceTex);
}

uint64_t SSRResolvePass::GetReprojDepthSrv() const
{
    if (!m_gfx || !m_reprojDepthTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_reprojDepthTex);
}

void SSRResolvePass::PreSnapshotCopy(RHI::CommandList cl)
{
    if (!m_gfx || !m_snapshotTex.IsValid()) return;
    if (m_snapshotState == RHI::ResourceState::COPY_DST) return;
    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_snapshotTex, m_snapshotState,
        RHI::ResourceState::COPY_DST), cl);
    m_snapshotState = RHI::ResourceState::COPY_DST;
}

void SSRResolvePass::PostSnapshotCopy(RHI::CommandList cl)
{
    if (!m_gfx || !m_snapshotTex.IsValid()) return;
    if (m_snapshotState == RHI::ResourceState::SHADER_RESOURCE) return;
    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_snapshotTex, m_snapshotState,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_snapshotState = RHI::ResourceState::SHADER_RESOURCE;
}

void SSRResolvePass::Execute(RHI::CommandList cl,
                             uint32_t w, uint32_t h,
                             uint64_t normalSrv, uint64_t surfaceSrv, uint64_t depthSrv,
                             uint64_t hitBufferSrv, uint64_t rayDirPDFSrv,
                             uint64_t rayLengthSrv)
{
    if (!m_pso.IsValid() || !m_cbMapped) return;
    if (w != m_w || h != m_h) return;
    if (!hitBufferSrv || !rayDirPDFSrv) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);

    SSRResolveCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    cb.cameraPos[0] = m_cam.cameraPos.x;
    cb.cameraPos[1] = m_cam.cameraPos.y;
    cb.cameraPos[2] = m_cam.cameraPos.z;
    cb.screenW      = w;
    cb.screenH      = h;
    cb.invScreenW   = 1.0f / float(w);
    cb.invScreenH   = 1.0f / float(h);
    cb.nearZ        = m_cam.nearZ;
    cb.farZ         = m_cam.farZ;
    cb.frameIndex   = m_frameIndex;
    cb.fireflyCap   = m_fireflyCap;
    std::memcpy(m_cbMapped, &cb, sizeof(cb));

    auto toUAV = [&](RHI::Texture& tex, RHI::ResourceState& state) {
        if (state != RHI::ResourceState::UNORDERED_ACCESS)
        {
            dx12.PushBarrier(RHI::GPUBarrier::Image(
                &tex, state, RHI::ResourceState::UNORDERED_ACCESS), cl);
            state = RHI::ResourceState::UNORDERED_ACCESS;
        }
    };
    auto toSR = [&](RHI::Texture& tex, RHI::ResourceState& state) {
        if (state != RHI::ResourceState::SHADER_RESOURCE)
        {
            dx12.PushBarrier(RHI::GPUBarrier::Image(
                &tex, state, RHI::ResourceState::SHADER_RESOURCE), cl);
            state = RHI::ResourceState::SHADER_RESOURCE;
        }
    };

    toUAV(m_colorTex,       m_colorState);
    toUAV(m_varianceTex,    m_varianceState);
    toUAV(m_reprojDepthTex, m_reprojDepthState);

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb, 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, normalSrv,     cl);  // t0
    dx12.SetComputeDescriptorTable(kSRV_T1, surfaceSrv,    cl);  // t1
    dx12.SetComputeDescriptorTable(kSRV_T2, depthSrv,      cl);  // t2
    dx12.SetComputeDescriptorTable(kSRV_T3, hitBufferSrv,  cl);  // t3
    dx12.SetComputeDescriptorTable(kSRV_T4, rayDirPDFSrv,  cl);  // t4
    if (rayLengthSrv)
        dx12.SetComputeDescriptorTable(kSRV_T5, rayLengthSrv, cl); // t5
    dx12.SetComputeDescriptorTable(kUAV_U0,
        m_gfx->GetTextureUAVGpuHandle(m_colorTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U1,
        m_gfx->GetTextureUAVGpuHandle(m_varianceTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U2,
        m_gfx->GetTextureUAVGpuHandle(m_reprojDepthTex), cl);

    const uint32_t gx = (w + 7) / 8;
    const uint32_t gy = (h + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);

    toSR(m_colorTex,       m_colorState);
    toSR(m_varianceTex,    m_varianceState);
    toSR(m_reprojDepthTex, m_reprojDepthState);
}

// ===========================================================================
// SSRTemporalPass — dual-reprojection history accumulation
// ===========================================================================
namespace
{
    struct alignas(16) SSRTemporalCB
    {
        float    invViewProj[16];   // jittered (matches depth rasterization)
        float    prevViewProj[16];  // prev-frame jittered (matches history grid)
        uint32_t screenW;     uint32_t screenH;
        float    invScreenW;  float    invScreenH;
        uint32_t resetHistory;
        float    nearZ;       float    farZ;
        float    _pad0;
    };
    static_assert(sizeof(SSRTemporalCB) <= 256, "SSRTemporalCB > 256");
}

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

    RHI::GPUBufferDesc bd{};
    bd.size = 256;
    bd.usage = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    LOG_SUCCESS("SSRTemporalPass: initialised");
}

void SSRTemporalPass::EnsureTextures(uint32_t w, uint32_t h)
{
    if (!m_gfx || w == 0 || h == 0) return;
    if (w == m_w && h == m_h &&
        m_color[0].IsValid() && m_color[1].IsValid() &&
        m_variance[0].IsValid() && m_variance[1].IsValid() &&
        m_depthHistory[0].IsValid() && m_depthHistory[1].IsValid())
        return;

    for (auto& t : m_color)        if (t.IsValid()) m_gfx->DestroyTexture(t);
    for (auto& t : m_variance)     if (t.IsValid()) m_gfx->DestroyTexture(t);
    for (auto& t : m_depthHistory) if (t.IsValid()) m_gfx->DestroyTexture(t);

    m_w = w; m_h = h;
    m_writeIdx = 0;
    m_resetHistory = true;

    RHI::TextureDesc rgba{};
    rgba.width = w; rgba.height = h;
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

    LOG_INFO("SSRTemporalPass: temporal buffers %ux%u ready", w, h);
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
                              uint32_t w, uint32_t h,
                              uint64_t colorCurrentSrv,
                              uint64_t varianceCurrentSrv,
                              uint64_t reprojDepthSrv,
                              uint64_t velocitySrv,
                              uint64_t depthSrv)
{
    if (!m_pso.IsValid() || !m_cbMapped) return;
    if (w != m_w || h != m_h) return;
    if (!colorCurrentSrv) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    const uint32_t readIdx = 1u - m_writeIdx;

    SSRTemporalCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.prevViewProj)));
    cb.screenW = w; cb.screenH = h;
    cb.invScreenW = 1.0f / float(w);
    cb.invScreenH = 1.0f / float(h);
    cb.resetHistory = m_resetHistory ? 1u : 0u;
    cb.nearZ = m_cam.nearZ;
    cb.farZ  = m_cam.farZ;
    std::memcpy(m_cbMapped, &cb, sizeof(cb));

    // Output SR → UAV. depthHistory[writeIdx] is also a UAV write target this
    // dispatch; depthHistory[readIdx] stays in SHADER_RESOURCE so the same
    // resource isn't bound as both SRV and UAV in one dispatch.
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
    dx12.SetComputeRootCBV(kCB, m_cb, 0, cl);
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
        m_gfx->GetTextureSRVGpuHandle(m_depthHistory[readIdx]), cl);    // t7 (prev)

    dx12.SetComputeDescriptorTable(kUAV_U0,
        m_gfx->GetTextureUAVGpuHandle(m_color[m_writeIdx]), cl);
    dx12.SetComputeDescriptorTable(kUAV_U1,
        m_gfx->GetTextureUAVGpuHandle(m_variance[m_writeIdx]), cl);
    dx12.SetComputeDescriptorTable(kUAV_U2,
        m_gfx->GetTextureUAVGpuHandle(m_depthHistory[m_writeIdx]), cl); // u2 (curr)

    const uint32_t gx = (w + 7) / 8;
    const uint32_t gy = (h + 7) / 8;
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

// ===========================================================================
// SSRUpsamplePass — variance-driven bilateral blur (final SSR output)
// ===========================================================================
namespace
{
    struct alignas(16) SSRUpsampleCB
    {
        float    invViewProj[16];
        uint32_t screenW;     uint32_t screenH;
        float    invScreenW;  float    invScreenH;
        float    nearZ;       float    farZ;
        float    _pad0;       float    _pad1;
    };
    static_assert(sizeof(SSRUpsampleCB) <= 256, "SSRUpsampleCB > 256");
}

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

    RHI::GPUBufferDesc bd{};
    bd.size = 256;
    bd.usage = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    LOG_SUCCESS("SSRUpsamplePass: initialised");
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
    if (!m_pso.IsValid() || !m_cbMapped) return;
    if (w != m_w || h != m_h) return;
    if (!temporalSrv || !varianceSrv) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);

    SSRUpsampleCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    cb.screenW = w; cb.screenH = h;
    cb.invScreenW = 1.0f / float(w);
    cb.invScreenH = 1.0f / float(h);
    cb.nearZ = m_cam.nearZ;
    cb.farZ  = m_cam.farZ;
    std::memcpy(m_cbMapped, &cb, sizeof(cb));

    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_colorTex, RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb, 0, cl);
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

// ===========================================================================
// SSRCompositePass — additive reflection composite
// ===========================================================================
namespace
{
    struct alignas(16) SSRCompositeCB
    {
        float    invViewProj[16];
        float    cameraPos[3];   float    _pad0;
        uint32_t screenW;        uint32_t screenH;
        float    intensity;      uint32_t debugMode;
        float    roughnessCutoff; float   _pad1;
    };
}

void SSRCompositePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRComposite_CS, RHI::ShaderStage::CS,
                         "SSRComposite.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRComposite_CS);
    if (!cs) { LOG_ERROR("SSRCompositePass: SSRComposite_CS not found"); return; }
    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRCompositePass: PSO creation failed"); return; }

    RHI::GPUBufferDesc bd{};
    bd.size       = 256;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    LOG_SUCCESS("SSRCompositePass: initialised");
}

void SSRCompositePass::Execute(RHI::CommandList cl,
                                uint32_t w, uint32_t h,
                                uint64_t albedoSrv, uint64_t normalSrv, uint64_t surfaceSrv,
                                uint64_t depthSrv,  uint64_t ssrSrv,    uint64_t hdrUav,
                                uint64_t brdfLutSrv,
                                uint64_t rawTraceSrv, uint64_t rayDirSrv,
                                uint64_t rayLenSrv,   uint64_t varianceSrv)
{
    if (!m_pso.IsValid() || !m_cbMapped || !hdrUav) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);

    SSRCompositeCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    cb.cameraPos[0] = m_cam.cameraPos.x;
    cb.cameraPos[1] = m_cam.cameraPos.y;
    cb.cameraPos[2] = m_cam.cameraPos.z;
    cb.screenW         = w;
    cb.screenH         = h;
    cb.intensity       = m_intensity;
    cb.debugMode       = m_debugMode;
    cb.roughnessCutoff = m_roughnessCutoff;
    std::memcpy(m_cbMapped, &cb, sizeof(cb));

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb, 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, albedoSrv,  cl);  // t0
    dx12.SetComputeDescriptorTable(kSRV_T1, normalSrv,  cl);  // t1
    dx12.SetComputeDescriptorTable(kSRV_T2, surfaceSrv, cl);  // t2
    dx12.SetComputeDescriptorTable(kSRV_T3, depthSrv,   cl);  // t3
    dx12.SetComputeDescriptorTable(kSRV_T4, ssrSrv,     cl);  // t4
    dx12.SetComputeDescriptorTable(kSRV_T5, brdfLutSrv, cl);  // t5
    // Debug SRVs — only the shader's debugMode branches read these, so we
    // only bind if the caller routed them. Shared compute root-sig exposes
    // slots up to t7 in space2; extra debug views (ray length, variance)
    // are shown via ImGui::Image tiles in the editor window instead.
    if (rawTraceSrv)  dx12.SetComputeDescriptorTable(kSRV_T6, rawTraceSrv, cl);  // t6
    if (rayDirSrv)    dx12.SetComputeDescriptorTable(kSRV_T7, rayDirSrv,   cl);  // t7
    (void)rayLenSrv;  (void)varianceSrv;  // reserved for future root-sig extension
    dx12.SetComputeDescriptorTable(kUAV_U0, hdrUav,     cl);

    const uint32_t gx = (w + 7) / 8;
    const uint32_t gy = (h + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);
}
