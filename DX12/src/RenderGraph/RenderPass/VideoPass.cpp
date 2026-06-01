#include "RenderGraph/RenderPass/VideoPass.h"
#include "ECS/VideoComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>

// Default-root-sig slots that the composite PS references:
//   t6 space0 — Y plane SRV  → root slot 19
//   t7 space0 — UV plane SRV → root slot 20
//   b2 space0 — CB           → BindConstantBuffer(slot=1) (slot 0 = b1)
static constexpr uint32_t kYPlaneRootSlot   = 19;
static constexpr uint32_t kUVPlaneRootSlot  = 20;
static constexpr uint32_t kCompositeCBSlot  = 1;   // b2 via root[2]

// ---------------------------------------------------------------------------
VideoPass::VideoPass() = default;

VideoPass::~VideoPass()
{
    if (m_gfx)
        m_cb.Destroy(*m_gfx);
}

// ---------------------------------------------------------------------------
void VideoPass::Setup(RG::RenderGraphBuilder& b)
{
    // Do NOT declare HdrSceneColor as the colour target — the graph would
    // clear HDR on entry, wiping Lighting / Skybox / Clouds before we blend
    // on top. Bind HDR manually inside Execute, mirroring CloudPass.
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// ---------------------------------------------------------------------------
void VideoPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Shaders ----------------------------------------------------------
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VideoComposite_VS, RHI::ShaderStage::VS,
                         "VideoComposite.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::VideoComposite_PS, RHI::ShaderStage::PS,
                         "VideoComposite.ps.hlsl", "main");

    // ---- PSO cache -------------------------------------------------------
    m_psoCache.Init(gfx, m_shaderLib);
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VideoComposite_VS;
        desc.psID        = ShaderID::VideoComposite_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cull_mode         = RHI::CullMode::NONE;
        desc.rs.depth_clip_enable = false;
        desc.dss.depth_enable     = false;
        desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
        // Standard SRC_ALPHA over blend — alpha=1 fully replaces the dst,
        // alpha<1 lets the under-content show through (UI / overlay use).
        auto& bs = desc.bs.render_target[0];
        bs.blend_enable             = true;
        bs.src_blend                = RHI::Blend::SRC_ALPHA;
        bs.dest_blend               = RHI::Blend::INV_SRC_ALPHA;
        bs.blend_op                 = RHI::BlendOp::ADD;
        bs.src_blend_alpha          = RHI::Blend::ONE;
        bs.dest_blend_alpha         = RHI::Blend::INV_SRC_ALPHA;
        bs.blend_op_alpha           = RHI::BlendOp::ADD;
        bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;

        if (m_psoCache.GetOrCreate(desc))
            m_psoCreated = true;
        else
            LOG_ERROR("VideoPass: composite PSO create failed");
    }

    // ---- Constant buffer + sampler ---------------------------------------
    m_cb.Create(gfx, "Video.CompositeCB");

    RHI::SamplerDesc sd;
    sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
    sd.address_u = RHI::TextureAddressMode::CLAMP;
    sd.address_v = RHI::TextureAddressMode::CLAMP;
    sd.address_w = RHI::TextureAddressMode::CLAMP;
    gfx.CreateSampler(sd, m_linearSamplerSlot);

    LOG_SUCCESS("VideoPass: initialised");
}

// ---------------------------------------------------------------------------
void VideoPass::SetActiveFrame(const VideoComponent* vc)
{
    m_activeFrame = vc;
}

void VideoPass::SetRect(float uMin, float vMin, float uMax, float vMax)
{
    m_rectUV[0] = uMin; m_rectUV[1] = vMin;
    m_rectUV[2] = uMax; m_rectUV[3] = vMax;
}

// ---------------------------------------------------------------------------
RHI::CommandList VideoPass::Execute(RHI::CommandList cl)
{
    if (!m_gfx || !m_psoCreated)                       return cl;
    if (!m_activeFrame)                                return cl;
    if (m_activeFrame->currentDpbSlot >= m_activeFrame->dpb.size()) return cl;

    const RHI::Texture& nv12 =
        m_activeFrame->dpb[m_activeFrame->currentDpbSlot];
    if (!nv12.IsValid())                               return cl;

    IGraphicsDevice& gfx = *m_gfx;

    // ---- Upload CB --------------------------------------------------------
    if (auto* slot = m_cb.Current(gfx))
    {
        VideoCompositeCB c{};
        std::memcpy(c.rectUV, m_rectUV, sizeof(c.rectUV));
        c.alpha      = m_alpha;
        c.colorSpace = m_colorSpace;
        *slot = c;
    }

    // ---- Bind HDR colour as RT (no clear) --------------------------------
    // Pass nullptr depth — the composite has depth test off; no need to bind
    // the scene depth at all. SetRenderTargetToHdrWithDepth accepts null DSV.
    gfx.SetRenderTargetToHdrWithDepth(nullptr, cl);

    cl.BindDescriptorHeaps();

    const PSODesc desc = [this]{
        PSODesc d;
        d.vsID        = ShaderID::VideoComposite_VS;
        d.psID        = ShaderID::VideoComposite_PS;
        d.inputLayout = InputLayoutType::None;
        d.rs.cull_mode         = RHI::CullMode::NONE;
        d.rs.depth_clip_enable = false;
        d.dss.depth_enable     = false;
        d.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
        auto& bs = d.bs.render_target[0];
        bs.blend_enable             = true;
        bs.src_blend                = RHI::Blend::SRC_ALPHA;
        bs.dest_blend               = RHI::Blend::INV_SRC_ALPHA;
        bs.blend_op                 = RHI::BlendOp::ADD;
        bs.src_blend_alpha          = RHI::Blend::ONE;
        bs.dest_blend_alpha         = RHI::Blend::INV_SRC_ALPHA;
        bs.blend_op_alpha           = RHI::BlendOp::ADD;
        bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
        d.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        d.rtvCount      = 1;
        d.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;
        return d;
    }();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return cl;

    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    // CB → b2 space0
    gfx.BindConstantBuffer(m_cb.CurrentBuffer(gfx), kCompositeCBSlot, cl);

    // Y plane → t6 space0 (root slot 19)
    cl.BindDescriptorTableHandle(kYPlaneRootSlot,
        gfx.GetTextureSRVGpuHandle(nv12));
    // UV plane → t7 space0 (root slot 20)
    cl.BindDescriptorTableHandle(kUVPlaneRootSlot,
        gfx.GetTextureUVPlaneSRVGpuHandle(nv12));

    if (m_linearSamplerSlot >= 0)
        cl.BindSampler(0, m_linearSamplerSlot);

    cl.DrawInstanced(3, 1, 0, 0);
    return cl;
}
