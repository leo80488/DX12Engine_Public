#include "RenderGraph/RenderPass/GrassPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>

// Root parameter slots — must match GraphicsDX12.cpp's default root signature.
//   [10] descriptor table, t2 space0 ← per-draw SRV slot 0 (heightmap, ALL vis)
static constexpr uint32_t kHeightmapRootSlot = 10;

GrassPass::GrassPass(RG::RGTextureHandle albedo,
                     RG::RGTextureHandle normal,
                     RG::RGTextureHandle surface,
                     RG::RGTextureHandle depth,
                     RG::RGTextureHandle velocity,
                     RG::RGTextureHandle emissive)
    : m_albedo(albedo), m_normal(normal), m_surface(surface)
    , m_depth(depth), m_velocity(velocity), m_emissive(emissive)
{}

// ---------------------------------------------------------------------------
void GrassPass::Setup(RG::RenderGraphBuilder& b)
{
    // Same writes as GBufferPass/TerrainPass — grass shares the deferred
    // GBuffer and depth-tests against scene + terrain (registration order in
    // Renderer::Compile puts this pass right after TerrainPass).
    b.WriteRenderTarget(m_albedo);
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    b.WriteRenderTarget(m_velocity);
    b.WriteRenderTarget(m_emissive);
    b.WriteDepthStencil(m_depth);
}

// ---------------------------------------------------------------------------
bool GrassPass::BuildPSO(IGraphicsDevice& gfx, bool wireframe, RHI::PipelineState& outPso)
{
    const RHI::Shader* as = m_shaderLib.GetShader(ShaderID::Grass_AS);
    const RHI::Shader* ms = m_shaderLib.GetShader(ShaderID::Grass_MS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Grass_PS);
    if (!as || !as->IsValid()) { LOG_ERROR("GrassPass: Grass_AS not found"); return false; }
    if (!ms || !ms->IsValid()) { LOG_ERROR("GrassPass: Grass_MS not found"); return false; }
    if (!ps || !ps->IsValid()) { LOG_ERROR("GrassPass: Grass_PS not found"); return false; }

    RHI::RasterizerState rs{};
    rs.fill_mode = wireframe ? RHI::FillMode::WIREFRAME : RHI::FillMode::SOLID;
    // Blades are single-sided ribbons viewed from both sides; the MS orients
    // each vertex normal toward the camera (before its up-blend), so no
    // PS-side SV_IsFrontFace flip exists — exactly one mechanism.
    rs.cull_mode = RHI::CullMode::NONE;
    rs.front_counter_clockwise = false;
    rs.depth_clip_enable = true;

    RHI::DepthStencilState dss{};
    dss.depth_enable     = true;
    dss.depth_write_mask = RHI::DepthWriteMask::ALL;
    dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;   // reversed-Z
    // Stencil REPLACE ref 1 (PBR) — LightingPass's PBR branch (EQUAL, ref=1)
    // must shade grass pixels, exactly like TerrainPass.
    dss.stencil_enable      = true;
    dss.stencil_read_mask   = 0xFF;
    dss.stencil_write_mask  = 0xFF;
    dss.front_face.stencil_pass_op = RHI::StencilOp::REPLACE;
    dss.front_face.stencil_func    = RHI::ComparisonFunc::ALWAYS;
    dss.back_face = dss.front_face;

    RHI::BlendState bs{};
    for (int i = 0; i < 6; ++i)
        bs.render_target[i].render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    RHI::PipelineStateDesc pd{};
    pd.as  = as;
    pd.ms  = ms;
    pd.ps  = ps;
    pd.rs  = &rs;
    pd.dss = &dss;
    pd.bs  = &bs;
    pd.il  = nullptr;
    pd.pt  = RHI::PrimitiveTopology::TRIANGLELIST;   // ignored for mesh-shader PSO
    pd.rtv_count      = 6;
    pd.rtv_formats[0] = RHI::Format::R11G11B10_FLOAT;       // albedo
    pd.rtv_formats[1] = RHI::Format::R16G16B16A16_FLOAT;    // normal
    pd.rtv_formats[2] = RHI::Format::R8G8B8A8_UNORM;        // surface
    pd.rtv_formats[3] = RHI::Format::R16G16_FLOAT;          // velocity
    pd.rtv_formats[4] = RHI::Format::R16G16B16A16_FLOAT;    // extra
    pd.rtv_formats[5] = RHI::Format::R16G16B16A16_FLOAT;    // HdrSceneColor (emissive seed)
    pd.dsv_format     = RHI::Format::D32_FLOAT_S8X24_UINT;
    pd.sample_count   = 1;

    if (!gfx.CreatePipelineState(pd, outPso))
    {
        LOG_ERROR("GrassPass: PSO creation failed");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
void GrassPass::Init(IGraphicsDevice& gfx)
{
    auto* dx12 = dynamic_cast<GraphicsDX12*>(&gfx);
    m_meshShaderAvailable = dx12 ? dx12->SupportsMeshShader() : false;
    if (!m_meshShaderAvailable)
    {
        LOG_WARNING("GrassPass: device reports no mesh-shader support — pass disabled");
        return;
    }

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Grass_AS, RHI::ShaderStage::AS, "Grass.as.hlsl");
    m_shaderLib.Register(ShaderID::Grass_MS, RHI::ShaderStage::MS, "Grass.ms.hlsl");
    m_shaderLib.Register(ShaderID::Grass_PS, RHI::ShaderStage::PS, "Grass.ps.hlsl");

    // Always-needed resources FIRST — a shader compile failure below must not
    // leave the CB/sampler/fallback missing, or a later hot-reload that fixes
    // the shader would dispatch with b2 unbound (undefined constants).

    // s0 — trilinear-clamp for the heightmap (UV ∈ [0,1], matches TerrainPass).
    {
        RHI::SamplerDesc sd{};
        sd.filter         = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u      = RHI::TextureAddressMode::CLAMP;
        sd.address_v      = RHI::TextureAddressMode::CLAMP;
        sd.address_w      = RHI::TextureAddressMode::CLAMP;
        sd.max_anisotropy = 1;
        if (!gfx.CreateSampler(sd, m_clampSamplerIdx))
            LOG_ERROR("GrassPass: failed to create linear-clamp sampler");
    }

    // Triple-buffered GrassCB (b2) — registered with the graph each frame by
    // Renderer under the name "GrassParams".
    if (!m_grassCB.Create(gfx, "GrassPass.GrassCB"))
        LOG_ERROR("GrassPass: GrassCB creation failed");

    // 1×1 R16_UNORM zero heightmap so root slot 10 always has a valid
    // Texture2D descriptor (flat-field mode / heightmap still loading).
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R16_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint16_t zero = 0;
        RHI::SubresourceData init{ &zero, 2, 2 };
        if (gfx.CreateTexture(td, m_fallbackHeightmap, &init))
            m_fallbackHeightmapSRV = gfx.GetTextureSRVGpuHandle(m_fallbackHeightmap);
        else
            LOG_ERROR("GrassPass: fallback heightmap creation failed");
    }

    if (!BuildPSO(gfx, /*wireframe*/false, m_pso))
        return;
    BuildPSO(gfx, /*wireframe*/true, m_psoWire);   // non-fatal

    LOG_SUCCESS("GrassPass: initialized");
}

// ---------------------------------------------------------------------------
void GrassPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_meshShaderAvailable) return;
    m_shaderLib.ClearCaches();
    BuildPSO(gfx, /*wireframe*/false, m_pso);
    BuildPSO(gfx, /*wireframe*/true,  m_psoWire);
}

// ---------------------------------------------------------------------------
RHI::CommandList GrassPass::Execute(RHI::CommandList cl)
{
    if (!m_meshShaderAvailable || !m_pso.IsValid()) return cl;
    if (m_field.dispatchAsGroupCount == 0)          return cl;
    if (!m_grassCB.IsValid())                       return cl;   // CB never created

    auto& gfx = cl.GetDevice();

    // Upload this frame's CB (pushed by Renderer in BuildScene_SyncGrass).
    if (auto* slot = m_grassCB.Current(gfx))
        std::memcpy(slot, &m_pendingCB, sizeof(m_pendingCB));

    // GBufferPass already cleared the RTs; just (re)attach them.
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();

    cl.SetPipelineState((m_wireframe && m_psoWire.IsValid()) ? m_psoWire : m_pso);
    cl.BindDescriptorHeaps();

    cl.BindCBByName(0, "PerView");       // b1
    cl.BindCBByName(1, "GrassParams");   // b2

    const uint64_t hmSRV = m_field.heightmapSRV ? m_field.heightmapSRV
                                                : m_fallbackHeightmapSRV;
    if (!hmSRV) return cl;
    cl.BindDescriptorTableHandle(kHeightmapRootSlot, hmSRV);

    if (m_clampSamplerIdx >= 0)
        cl.BindSampler(0, m_clampSamplerIdx);

    // Stencil ref 1 (PBR) — match LightingPass::kStencilPBR.
    cl.gfx->SetStencilRef(1u, cl);

    cl.DispatchMesh(m_field.dispatchAsGroupCount, 1, 1);
    return cl;
}
