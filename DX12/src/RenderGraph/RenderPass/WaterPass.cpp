#include "RenderGraph/RenderPass/WaterPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>

// Root parameter slots — must match GraphicsDX12.cpp's default root signature.
//   [10] descriptor table, t2 space0  ← terrain heightmap (ALL vis)
//   [11] descriptor table, t3 space0  ← sky TextureCube  (ALL vis)
//   [12] descriptor table, t4 space0  ← flow-normal map A (ALL vis)
//   [13] descriptor table, t5 space0  ← flow-normal map B (ALL vis)
//   [22] descriptor table, t9..t11 space0 ← CSM cascade array (PIXEL vis)
//   [41] descriptor table, t27 space0 ← prev-frame SSR result (PIXEL vis)
static constexpr uint32_t kHeightmapRootSlot = 10;
static constexpr uint32_t kSkyCubeRootSlot   = 11;
static constexpr uint32_t kNormalARootSlot   = 12;
static constexpr uint32_t kNormalBRootSlot   = 13;
static constexpr uint32_t kShadowSRVSlot     = 22;
static constexpr uint32_t kSSRResultRootSlot = 41;

WaterPass::WaterPass(RG::RGTextureHandle normal,
                     RG::RGTextureHandle surface,
                     RG::RGTextureHandle velocity,
                     RG::RGTextureHandle depth)
    : m_normal(normal), m_surface(surface), m_velocity(velocity), m_depth(depth)
{}

// ---------------------------------------------------------------------------
void WaterPass::Setup(RG::RenderGraphBuilder& b)
{
    // GBuffer normal + surface as writes: the water stamps its flow normal /
    // roughness so the post-frame SSR chain traces off the water surface
    // (OutlinePass's ReadSRV(normal) restores SHADER_RESOURCE afterwards;
    // SSRSubsystem is state-aware via RenderGraph::GetTextureState).
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    // Velocity too: without it, TAA / SSR-temporal reproject water pixels
    // with the motion vectors of whatever the GBuffer pass drew UNDERNEATH
    // (submerged terrain / shoreline grass blades) — ghosting over water,
    // shimmer at the grass/water boundary, and reflection seams under
    // vertical camera motion. The plane is static so its velocity is pure
    // camera reprojection delta. (GBuffer3's graph-end state stays
    // RENDER_TARGET — same as before water wrote it, no restore needed.)
    b.WriteRenderTarget(m_velocity);
    // Depth as write: hardware test + depth WRITE (the water surface is real
    // geometry for the later depth-aware passes). HDR RTV bound manually.
    b.WriteDepthStencil(m_depth);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// ---------------------------------------------------------------------------
void WaterPass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Water_VS, RHI::ShaderStage::VS, "Water.vs.hlsl");
    m_shaderLib.Register(ShaderID::Water_PS, RHI::ShaderStage::PS, "Water.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    if (!m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_ERROR("WaterPass: PSO creation failed");

    // s0 — trilinear clamp: heightmap UV ∈ [0,1]; cube sampling ignores
    // addressing for the major axis anyway.
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        if (!gfx.CreateSampler(sd, m_clampSamplerIdx))
            LOG_ERROR("WaterPass: failed to create clamp sampler");
    }

    // s1 — anisotropic wrap: flow-normal maps tile across world XZ (UV runs
    // over many integer periods) and the water plane is viewed at grazing
    // angles, where anisotropy keeps the wave detail crisp.
    {
        RHI::SamplerDesc sd{};
        sd.filter         = RHI::Filter::ANISOTROPIC;
        sd.address_u      = RHI::TextureAddressMode::WRAP;
        sd.address_v      = RHI::TextureAddressMode::WRAP;
        sd.address_w      = RHI::TextureAddressMode::WRAP;
        sd.max_anisotropy = 8;
        if (!gfx.CreateSampler(sd, m_wrapSamplerIdx))
            LOG_ERROR("WaterPass: failed to create wrap sampler");
    }

    // s2 — PCF comparison sampler for the CSM cascades (reversed-Z →
    // GREATER_EQUAL, border white = lit). Mirrors LightingPass.
    {
        RHI::SamplerDesc sd{};
        sd.filter          = RHI::Filter::COMPARISON_MIN_MAG_MIP_LINEAR;
        sd.address_u       = RHI::TextureAddressMode::BORDER;
        sd.address_v       = RHI::TextureAddressMode::BORDER;
        sd.address_w       = RHI::TextureAddressMode::BORDER;
        sd.border_color    = RHI::SamplerBorderColor::OPAQUE_WHITE;
        sd.comparison_func = RHI::ComparisonFunc::GREATER_EQUAL;
        if (!gfx.CreateSampler(sd, m_shadowSamplerIdx))
            LOG_ERROR("WaterPass: shadow comparison sampler creation failed");
    }

    if (!m_waterCB.Create(gfx, "WaterPass.WaterCB"))
        LOG_ERROR("WaterPass: WaterCB creation failed");

    // 1×1 R16_UNORM zero heightmap — valid t2 descriptor when no terrain.
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R16_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint16_t zero = 0;
        RHI::SubresourceData init{ &zero, 2, 2 };
        if (gfx.CreateTexture(td, m_fallbackHeightmap, &init))
            m_fallbackHeightmapSRV = gfx.GetTextureSRVGpuHandle(m_fallbackHeightmap);
        else
            LOG_ERROR("WaterPass: fallback heightmap creation failed");
    }

    // 1×1 flat tangent-space normal (128,128,255) — calm water while the
    // flow-normal maps load (or when paths are cleared).
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t flat = 0xFFFF8080u;   // ABGR: A=FF B=FF(z) G=80 R=80
        RHI::SubresourceData init{ &flat, 4, 4 };
        if (gfx.CreateTexture(td, m_fallbackFlatNormal, &init))
            m_fallbackFlatNormalSRV = gfx.GetTextureSRVGpuHandle(m_fallbackFlatNormal);
        else
            LOG_ERROR("WaterPass: fallback flat-normal creation failed");
    }

    // 1×1 black — t27 placeholder when SSR is off (conf reads 0 → no-op).
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t black = 0x00000000u;
        RHI::SubresourceData init{ &black, 4, 4 };
        if (gfx.CreateTexture(td, m_fallbackBlack, &init))
            m_fallbackBlackSRV = gfx.GetTextureSRVGpuHandle(m_fallbackBlack);
        else
            LOG_ERROR("WaterPass: fallback black texture creation failed");
    }

    LOG_SUCCESS("WaterPass: initialized");
}

// ---------------------------------------------------------------------------
PSODesc WaterPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::Water_VS;
    desc.psID        = ShaderID::Water_PS;
    desc.inputLayout = InputLayoutType::None;   // SV_VertexID grid, no IA

    // Cull NONE — the plane must render from below too (underwater camera).
    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.fill_mode         = RHI::FillMode::SOLID;
    desc.rs.depth_clip_enable = true;

    // Reversed-Z test against scene depth; WRITE so clouds/fog see the
    // surface. Shoreline pixels with ~zero alpha are discarded in the PS
    // (clip) so they don't stamp depth / GBuffer either.
    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ALL;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;

    // RT layout (SetRenderTargetsAndHdr appends HDR LAST):
    //   RT0 = GBuffer1 normal   (opaque overwrite — SSR trace input)
    //   RT1 = GBuffer2 surface  (opaque overwrite — SSR roughness/Fresnel)
    //   RT2 = GBuffer3 velocity (opaque overwrite — TAA/SSR-temporal motion)
    //   RT3 = HDR scene color   (alpha blend — shoreline fade)
    desc.bs.independent_blend_enable = true;
    auto& rtN = desc.bs.render_target[0];
    rtN.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    rtN.blend_enable             = false;
    auto& rtS = desc.bs.render_target[1];
    rtS.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    rtS.blend_enable             = false;
    auto& rtV = desc.bs.render_target[2];
    rtV.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    rtV.blend_enable             = false;
    auto& rt = desc.bs.render_target[3];
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    rt.blend_enable             = true;
    rt.src_blend        = RHI::Blend::SRC_ALPHA;
    rt.dest_blend       = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op         = RHI::BlendOp::ADD;
    rt.src_blend_alpha  = RHI::Blend::ONE;
    rt.dest_blend_alpha = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op_alpha   = RHI::BlendOp::ADD;

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;   // GBuffer1 normal
    desc.rtvFormats[1] = RHI::Format::R8G8B8A8_UNORM;       // GBuffer2 surface
    desc.rtvFormats[2] = RHI::Format::R16G16_FLOAT;         // GBuffer3 velocity
    desc.rtvFormats[3] = RHI::Format::R16G16B16A16_FLOAT;   // HDR
    desc.rtvCount      = 4;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

// ---------------------------------------------------------------------------
RHI::CommandList WaterPass::Execute(RHI::CommandList cl)
{
    if (m_viewModeHidden)                 return cl;
    if (!m_water.active || !m_skyCubeSRV) return cl;
    if (!m_waterCB.IsValid())             return cl;   // CB never created

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc());
    if (!pso || !pso->IsValid()) return cl;

    auto& gfx = cl.GetDevice();

    // Upload this frame's CB (pushed by Renderer in BuildScene_SyncWater).
    if (auto* slot = m_waterCB.Current(gfx))
        std::memcpy(slot, &m_pendingCB, sizeof(m_pendingCB));

    // GBuffer normal + surface + velocity + HDR (appended last) + GBuffer
    // depth — all transitions already emitted from the Setup declarations.
    cl.SetRenderTargetsAndHdr({ m_normal, m_surface, m_velocity }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    cl.BindCBByName(0, "PerView");       // b1 (VS)
    cl.BindCBByName(1, "WaterParams");   // b2
    cl.BindCBByName(2, "LightCB");       // b3 (sun dir/color + cameraPos)

    const uint64_t hmSRV = m_water.heightmapSRV ? m_water.heightmapSRV
                                                : m_fallbackHeightmapSRV;
    if (!hmSRV) return cl;
    cl.BindDescriptorTableHandle(kHeightmapRootSlot, hmSRV);
    cl.BindDescriptorTableHandle(kSkyCubeRootSlot,   m_skyCubeSRV);

    // Flow-normal maps — flat fallback keeps t4/t5 valid while loading.
    cl.BindDescriptorTableHandle(kNormalARootSlot,
        m_water.normalASRV ? m_water.normalASRV : m_fallbackFlatNormalSRV);
    cl.BindDescriptorTableHandle(kNormalBRootSlot,
        m_water.normalBSRV ? m_water.normalBSRV : m_fallbackFlatNormalSRV);

    // Prev-frame SSR result (t27) — black fallback when SSR is disabled or
    // hasn't produced a frame yet (conf = 0 → sky-only reflection).
    cl.BindDescriptorTableHandle(kSSRResultRootSlot,
        m_ssrResultSRV ? m_ssrResultSRV : m_fallbackBlackSRV);

    // CSM cascade array (t9..t11) — sun-glint shadowing. Same guard style as
    // LightingPass: skip when absent (shadowStrength==0 keeps it unread).
    if (m_shadowSrvHandle)
        cl.BindDescriptorTableHandle(kShadowSRVSlot, m_shadowSrvHandle);

    if (m_clampSamplerIdx >= 0)
        cl.BindSampler(0, m_clampSamplerIdx);
    if (m_wrapSamplerIdx >= 0)
        cl.BindSampler(1, m_wrapSamplerIdx);
    if (m_shadowSamplerIdx >= 0)
        cl.BindSampler(2, m_shadowSamplerIdx);

    const uint32_t quads = m_pendingCB.gridQuads ? m_pendingCB.gridQuads : 1u;
    cl.DrawInstanced(quads * quads * 6u, 1, 0, 0);
    return cl;
}
