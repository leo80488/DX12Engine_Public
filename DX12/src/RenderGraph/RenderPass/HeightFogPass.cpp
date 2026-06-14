#include "RenderGraph/RenderPass/HeightFogPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
namespace
{
    // Fullscreen ONE/INV_SRC_ALPHA composite into HDR — identical to the
    // froxel VolumetricFogPass apply PSO, different PS.
    PSODesc MakeApplyDesc()
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VolumetricApply_VS;   // reuse fullscreen-tri VS
        desc.psID        = ShaderID::HeightFogApply_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cull_mode         = RHI::CullMode::NONE;
        desc.rs.depth_clip_enable = false;
        desc.dss.depth_enable     = false;
        desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
        auto& bs = desc.bs.render_target[0];
        bs.blend_enable     = true;
        bs.src_blend        = RHI::Blend::ONE;
        bs.dest_blend       = RHI::Blend::INV_SRC_ALPHA;
        bs.blend_op         = RHI::BlendOp::ADD;
        bs.src_blend_alpha  = RHI::Blend::ZERO;
        bs.dest_blend_alpha = RHI::Blend::ONE;
        bs.blend_op_alpha   = RHI::BlendOp::ADD;
        bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
        return desc;
    }
}

// ---------------------------------------------------------------------------
HeightFogPass::~HeightFogPass()
{
    if (m_gfx)
        m_fogCB.Destroy(*m_gfx);
}

// ---------------------------------------------------------------------------
void HeightFogPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth declared as SRV → graph emits the DEPTHSTENCIL → DEPTH_READ_SRV
    // barrier before this pass. Do NOT declare HdrSceneColor as the colour
    // target — the graph would CLEAR it on entry even when the pass is
    // disabled (same pattern as VolumetricFogPass / SkyboxPass).
    b.ReadSRV(m_depth);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// ---------------------------------------------------------------------------
void HeightFogPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VolumetricApply_VS, RHI::ShaderStage::VS,
                         "VolumetricApply.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::HeightFogApply_PS, RHI::ShaderStage::PS,
                         "HeightFogApply.ps.hlsl", "main");
    m_psoCache.Init(gfx, m_shaderLib);

    if (!m_psoCache.GetOrCreate(MakeApplyDesc()))
        LOG_ERROR("HeightFogPass: apply PSO failed");

    m_fogCB.Create(gfx, "HeightFogPass.FogCB");

    {
        RHI::SamplerDesc sd;
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        gfx.CreateSampler(sd, m_linearSampler);
    }

    LOG_SUCCESS("HeightFogPass: initialised");
}

// ---------------------------------------------------------------------------
RHI::CommandList HeightFogPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled || m_viewModeHidden) return cl;
    if (!m_gfx) return cl;
    IGraphicsDevice& gfx = *m_gfx;

    if (auto* slot = m_fogCB.Current(gfx))
    {
        HeightFogConstants c{};
        c.fogDensity            = m_params.fogDensity;
        c.fogHeightFalloff      = std::max(m_params.fogHeightFalloff, 0.0f);
        c.fogHeight             = m_params.fogHeight;
        c.startDistance         = std::max(m_params.startDistance, 0.0f);
        c.fogColor[0]           = m_params.fogColor.x;
        c.fogColor[1]           = m_params.fogColor.y;
        c.fogColor[2]           = m_params.fogColor.z;
        c.maxOpacity            = std::clamp(m_params.maxOpacity, 0.0f, 1.0f);
        c.sunInscatterIntensity = std::max(m_params.sunInscatterIntensity, 0.0f);
        c.anisotropy            = std::clamp(m_params.anisotropy, 0.0f, 0.99f);
        *slot = c;
    }

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(MakeApplyDesc());
    if (!pso || !pso->IsValid()) return cl;

    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (!depthTex) return cl;
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl);

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    cl.BindCBByName(0, "LightCB");       // b1 — sun dir/color, cameraPos, invViewProj
    cl.BindCBByName(1, "HeightFogCB");   // b2
    cl.BindSRVByHandle(3, m_depth);      // t5 space0 (root param 13)
    if (m_linearSampler >= 0) cl.BindSampler(0, m_linearSampler);

    uint32_t r = gfx.BeginGPUTimestamp(cl, "HeightFog.Apply");
    cl.DrawInstanced(3, 1, 0, 0);
    gfx.EndGPUTimestamp(cl, r);

    return cl;
}
