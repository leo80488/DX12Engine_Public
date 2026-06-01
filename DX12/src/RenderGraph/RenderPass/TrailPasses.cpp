#include "RenderGraph/RenderPass/TrailPasses.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/TrailSystem.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cstring>

// Compute root sig slots (same as ParticlePasses — compute root sig is shared).
static constexpr uint32_t kCSCBSlot       = 0;   // b0 space2
static constexpr uint32_t kCSRequestSRV   = 1;   // t0 space2 DESC_TABLE
static constexpr uint32_t kCSSegmentUAV   = 4;   // u0 space2 DESC_TABLE
static constexpr uint32_t kCSHeaderUAV    = 5;   // u1 space2 DESC_TABLE

// Graphics root sig slots (same as ParticlePasses render pass).
static constexpr uint32_t kGfxRenderCBSlot    = 2;  // ROOT_CBV b2 space0 (BindConstantBuffer slot 1)
static constexpr uint32_t kGfxSegmentSRVSlot  = 12; // DESC_TABLE t4 space0 (BindResource slot 2)
static constexpr uint32_t kGfxHeaderSRVSlot   = 13; // DESC_TABLE t5 space0 (BindResource slot 3)

// ===========================================================================
// TrailUpdatePass
// ===========================================================================

void TrailUpdatePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::TrailUpdate_CS, RHI::ShaderStage::CS,
                         "TrailUpdate.cs.hlsl", "CSMain");
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::TrailUpdate_CS);
    if (!cs)
    {
        LOG_ERROR("TrailUpdatePass: shader lookup failed");
        return;
    }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("TrailUpdatePass: PSO creation failed");

    LOG_SUCCESS("TrailUpdatePass: initialised");
}

void TrailUpdatePass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_pso.IsValid()) return;
    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Dispatch covers the max of maxTrails (aging pass) and requestCount
    // (append pass). Each thread decides its own work by comparing tid.
    const uint32_t threads = (std::max)(TrailSystem::kMaxTrails,
                                        m_sys->GetRequestCount());

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetSystemCB(*m_gfx), 0, cl);

    const uint64_t reqSrv = m_gfx->GetBufferSRVGpuHandle(m_sys->GetRequestBuffer(*m_gfx));
    gfx.SetComputeDescriptorTable(kCSRequestSRV, reqSrv, cl);
    gfx.SetComputeDescriptorTable(kCSSegmentUAV, m_sys->GetSegmentUAVHandle(), cl);
    gfx.SetComputeDescriptorTable(kCSHeaderUAV,  m_sys->GetHeaderUAVHandle(),  cl);

    gfx.DispatchCompute((threads + 63) / 64, 1, 1, cl);

    // UAV barriers so the render pass sees consistent state.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetSegmentBuffer()), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetHeaderBuffer()),  cl);
}

// ===========================================================================
// TrailRenderPass
// ===========================================================================

TrailRenderPass::TrailRenderPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

void TrailRenderPass::Setup(RG::RenderGraphBuilder& b)
{
    b.WriteDepthStencil(m_depth);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void TrailRenderPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Trail_VS, RHI::ShaderStage::VS, "Trail.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::Trail_PS, RHI::ShaderStage::PS, "Trail.ps.hlsl", "main");

    const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::Trail_VS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Trail_PS);
    if (!vs || !ps)
    {
        LOG_ERROR("TrailRenderPass: shader lookup failed");
        return;
    }

    // Graphics PSO: triangle list (6 verts per instance = 2 triangles),
    // cull none (ribbons are 2-sided), depth-test on / write off, alpha blend.
    RHI::RasterizerState   rs{};
    rs.fill_mode         = RHI::FillMode::SOLID;
    rs.cull_mode         = RHI::CullMode::NONE;
    rs.depth_clip_enable = true;

    RHI::DepthStencilState dss{};
    dss.depth_enable     = true;
    dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;

    RHI::BlendState bs{};
    auto& rt = bs.render_target[0];
    rt.blend_enable    = true;
    rt.src_blend       = RHI::Blend::SRC_ALPHA;
    rt.dest_blend      = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op        = RHI::BlendOp::ADD;
    rt.src_blend_alpha = RHI::Blend::ONE;
    rt.dest_blend_alpha= RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op_alpha  = RHI::BlendOp::ADD;
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    RHI::PipelineStateDesc d{};
    d.vs             = vs;
    d.ps             = ps;
    d.rs             = &rs;
    d.dss            = &dss;
    d.bs             = &bs;
    d.pt             = RHI::PrimitiveTopology::TRIANGLELIST;
    d.rtv_formats[0] = RHI::Format::R16G16B16A16_FLOAT;
    d.rtv_count      = 1;
    d.dsv_format     = RHI::Format::D24_UNORM_S8_UINT;
    d.sample_count   = 1;

    if (!gfx.CreatePipelineState(d, m_pso))
    {
        LOG_ERROR("TrailRenderPass: PSO creation failed");
        return;
    }

    if (!m_renderCB.Create(gfx, "TrailRenderPass.RenderCB"))
        LOG_ERROR("TrailRenderPass: render CB create failed");

    LOG_SUCCESS("TrailRenderPass: initialised");
}

RHI::CommandList TrailRenderPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_pso.IsValid() || !m_renderCB.IsValid()) return cl;
    if (m_sys->GetMaxActiveSlot() == 0) return cl;   // nothing to draw

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    using namespace DirectX;
    if (auto* slot = m_renderCB.Current(gfx))
    {
        RenderCB cb{};
        XMMATRIX vp = XMLoadFloat4x4(&m_viewProj);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),
                        XMMatrixTranspose(vp));
        cb.camForward[0] = m_camForward.x;
        cb.camForward[1] = m_camForward.y;
        cb.camForward[2] = m_camForward.z;
        cb.maxSegments   = TrailSystem::kMaxSegmentsPerTrail;
        *slot = cb;
    }

    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    gfx.SetRenderTargetToHdrWithDepth(depthTex, cl);

    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPipelineState(m_pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    gfx.BindConstantBuffer(m_renderCB.CurrentBuffer(gfx), /*slot=*/1, cl);
    cl.BindDescriptorTableHandle(kGfxSegmentSRVSlot, m_sys->GetSegmentSRVHandle());
    cl.BindDescriptorTableHandle(kGfxHeaderSRVSlot,  m_sys->GetHeaderSRVHandle());

    // 6 verts per segment-pair × (maxSegments-1) pairs × kMaxTrails
    const uint32_t vertsPerInstance = 6;
    const uint32_t instances = TrailSystem::kMaxTrails * (TrailSystem::kMaxSegmentsPerTrail - 1);
    gfx.DrawInstanced(vertsPerInstance, instances, 0, 0, cl);

    return cl;
}
