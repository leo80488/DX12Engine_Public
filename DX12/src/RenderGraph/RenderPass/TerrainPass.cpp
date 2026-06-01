#include "RenderGraph/RenderPass/TerrainPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

// Root parameter slots — must match GraphicsDX12.cpp's default root signature.
//   [10] descriptor table, t2 space0  ← per-draw SRV slot 0 (heightmap, ALL vis)
//   [11] descriptor table, t3 space0  ← per-draw SRV slot 1 (splatmap, ALL vis)
//   [27] descriptor table, t0 space2  ← bindless g_AllTextures[] (PIXEL vis)
static constexpr uint32_t kHeightmapRootSlot = 10;
static constexpr uint32_t kSplatmapRootSlot  = 11;
static constexpr uint32_t kBindlessTexSlot   = 27;

TerrainPass::TerrainPass(RG::RGTextureHandle albedo,
                         RG::RGTextureHandle normal,
                         RG::RGTextureHandle surface,
                         RG::RGTextureHandle depth,
                         RG::RGTextureHandle velocity,
                         RG::RGTextureHandle emissive)
    : m_albedo(albedo), m_normal(normal), m_surface(surface)
    , m_depth(depth), m_velocity(velocity), m_emissive(emissive)
{}

// ---------------------------------------------------------------------------
void TerrainPass::Setup(RG::RenderGraphBuilder& b)
{
    // Same writes as GBufferPass — terrain shares the deferred GBuffer.
    // The graph linearises both passes; ours runs strictly after GBufferPass
    // (registration order in Renderer::Compile) so depth-test against scene
    // geometry is correct.
    b.WriteRenderTarget(m_albedo);
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    b.WriteRenderTarget(m_velocity);
    b.WriteRenderTarget(m_emissive);
    b.WriteDepthStencil(m_depth);
}

// ---------------------------------------------------------------------------
bool TerrainPass::BuildPSO(IGraphicsDevice& gfx, bool wireframe, RHI::PipelineState& outPso)
{
    const RHI::Shader* as = m_shaderLib.GetShader(ShaderID::Terrain_AS);
    const RHI::Shader* ms = m_shaderLib.GetShader(ShaderID::Terrain_MS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Terrain_PS);
    if (!as || !as->IsValid())
    { LOG_ERROR("TerrainPass: Terrain_AS not found"); return false; }
    if (!ms || !ms->IsValid())
    { LOG_ERROR("TerrainPass: Terrain_MS not found"); return false; }
    if (!ps || !ps->IsValid())
    { LOG_ERROR("TerrainPass: Terrain_PS not found"); return false; }

    RHI::RasterizerState rs{};
    rs.fill_mode         = wireframe ? RHI::FillMode::WIREFRAME : RHI::FillMode::SOLID;
    rs.cull_mode         = RHI::CullMode::BACK;
    rs.front_counter_clockwise = false;
    rs.depth_clip_enable = true;

    RHI::DepthStencilState dss{};
    dss.depth_enable     = true;
    dss.depth_write_mask = RHI::DepthWriteMask::ALL;
    dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;   // reversed-Z (matches GBufferPass)
    // Stencil — REPLACE with the PBR shading-model id (1) so LightingPass's
    // PBR pass (stencil_func=EQUAL, ref=1) shades terrain pixels. Without
    // this, all three lighting branches skip terrain (stencil 0 ≠ 1/2/3),
    // the HDR target keeps last frame's contents, and the screen "stacks".
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
    pd.as = as;
    pd.ms = ms;
    pd.ps = ps;
    pd.rs  = &rs;
    pd.dss = &dss;
    pd.bs  = &bs;
    pd.il  = nullptr;
    pd.pt  = RHI::PrimitiveTopology::TRIANGLELIST;   // ignored for mesh-shader PSO
    pd.rtv_count    = 6;
    pd.rtv_formats[0] = RHI::Format::R11G11B10_FLOAT;       // albedo
    pd.rtv_formats[1] = RHI::Format::R16G16B16A16_FLOAT;    // normal
    pd.rtv_formats[2] = RHI::Format::R8G8B8A8_UNORM;        // surface
    pd.rtv_formats[3] = RHI::Format::R16G16_FLOAT;          // velocity
    pd.rtv_formats[4] = RHI::Format::R16G16B16A16_FLOAT;    // extra (shading-model scratch)
    pd.rtv_formats[5] = RHI::Format::R16G16B16A16_FLOAT;    // HdrSceneColor (direct emissive write)
    pd.dsv_format     = RHI::Format::D24_UNORM_S8_UINT;     // matches GBufferPass
    pd.sample_count   = 1;

    if (!gfx.CreatePipelineState(pd, outPso))
    {
        LOG_ERROR("TerrainPass: PSO creation failed");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
void TerrainPass::Init(IGraphicsDevice& gfx)
{
    // Mesh shaders are an optional DX12 feature. Without runtime support
    // we leave m_pso invalid and Execute becomes a silent no-op.
    auto* dx12 = dynamic_cast<GraphicsDX12*>(&gfx);
    m_meshShaderAvailable = dx12 ? dx12->SupportsMeshShader() : false;
    if (!m_meshShaderAvailable)
    {
        LOG_WARNING("TerrainPass: device reports no mesh-shader support — pass disabled");
        return;
    }

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Terrain_AS, RHI::ShaderStage::AS, "Terrain.as.hlsl");
    m_shaderLib.Register(ShaderID::Terrain_MS, RHI::ShaderStage::MS, "Terrain.ms.hlsl");
    m_shaderLib.Register(ShaderID::Terrain_PS, RHI::ShaderStage::PS, "Terrain.ps.hlsl");

    if (!BuildPSO(gfx, /*wireframe*/false, m_pso))
        return;
    // Wireframe variant for the global view-mode switch. Non-fatal if it
    // fails — Execute falls back to the solid PSO.
    BuildPSO(gfx, /*wireframe*/true, m_psoWire);

    // s0 — Trilinear-clamp for heightmap (MS) + splatmap (PS, sampled by the
    // tile-local UV ∈ [0,1]). Clamp avoids the four-tap derivative stencil
    // wrapping to the opposite edge of the heightmap.
    {
        RHI::SamplerDesc sd{};
        sd.filter         = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u      = RHI::TextureAddressMode::CLAMP;
        sd.address_v      = RHI::TextureAddressMode::CLAMP;
        sd.address_w      = RHI::TextureAddressMode::CLAMP;
        sd.max_anisotropy = 1;
        if (!gfx.CreateSampler(sd, m_clampSamplerIdx))
            LOG_ERROR("TerrainPass: failed to create linear-clamp sampler");
    }

    // s1 — Anisotropic-wrap for layer albedos. They're sampled at
    // worldXZ × per-layer tilingScale, so the UV runs over many integer
    // periods and MUST wrap to actually tile across the tile. Anisotropic
    // filtering keeps grazing-angle detail crisp (same setup as GBufferPass).
    {
        RHI::SamplerDesc sd{};
        sd.filter         = RHI::Filter::ANISOTROPIC;
        sd.address_u      = RHI::TextureAddressMode::WRAP;
        sd.address_v      = RHI::TextureAddressMode::WRAP;
        sd.address_w      = RHI::TextureAddressMode::WRAP;
        sd.max_anisotropy = 8;
        sd.mip_lod_bias   = -0.5f;     // mild bias for TAA-friendly detail
        if (!gfx.CreateSampler(sd, m_wrapSamplerIdx))
            LOG_ERROR("TerrainPass: failed to create anisotropic-wrap sampler");
    }

    LOG_SUCCESS("TerrainPass: initialized");
}

// ---------------------------------------------------------------------------
void TerrainPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_meshShaderAvailable) return;
    m_shaderLib.ClearCaches();
    BuildPSO(gfx, /*wireframe*/false, m_pso);
    BuildPSO(gfx, /*wireframe*/true,  m_psoWire);
}

// ---------------------------------------------------------------------------
RHI::CommandList TerrainPass::Execute(RHI::CommandList cl)
{
    if (!m_meshShaderAvailable || !m_pso.IsValid())                  return cl;
    if (m_tile.dispatchAsGroupCount == 0 || m_tile.heightmapSRV == 0) return cl;

    // GBufferPass already cleared the RTs (including HDR RT5); we only need to (re)attach them.
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();

    cl.SetPipelineState((m_wireframe && m_psoWire.IsValid()) ? m_psoWire : m_pso);
    cl.BindDescriptorHeaps();

    // CBs — slot index here is CB-relative; BindCBByName(0,…) → root b1, (1,…) → root b2.
    cl.BindCBByName(0, "PerView");
    cl.BindCBByName(1, "TerrainParams");

    // Heightmap SRV at t2 space0 (required).
    cl.BindDescriptorTableHandle(kHeightmapRootSlot, m_tile.heightmapSRV);

    // Splatmap SRV at t3 space0 (optional — PS branches on g_hasSplatmap).
    // When unset we still need a valid descriptor in the slot or D3D12
    // validation complains; reuse the heightmap as a benign placeholder.
    const uint64_t splatBind = m_tile.splatmapSRV ? m_tile.splatmapSRV
                                                  : m_tile.heightmapSRV;
    cl.BindDescriptorTableHandle(kSplatmapRootSlot, splatBind);

    // Bindless texture array for layer albedo lookups (PS only).
    auto& dx12 = static_cast<GraphicsDX12&>(*cl.gfx);
    if (D3D12_GPU_DESCRIPTOR_HANDLE bindlessTex = dx12.GetBindlessTextureTableHandle();
        bindlessTex.ptr != 0)
    {
        cl.BindDescriptorTableHandle(kBindlessTexSlot, bindlessTex.ptr);
    }

    // Samplers — s0 = clamp (heightmap/splatmap), s1 = wrap (layer albedos).
    if (m_clampSamplerIdx >= 0)
        cl.BindSampler(0, m_clampSamplerIdx);
    if (m_wrapSamplerIdx >= 0)
        cl.BindSampler(1, m_wrapSamplerIdx);

    // Stencil ref = 1 (PBR) — must match LightingPass's kStencilPBR so the
    // PBR lighting pass shades terrain pixels.
    cl.gfx->SetStencilRef(1u, cl);

    // DispatchMesh dispatches AS thread groups when an AS is bound; the AS
    // performs frustum culling per sub-tile and calls its own DispatchMesh
    // to emit only the surviving MS groups (see Terrain.as.hlsl).
    cl.DispatchMesh(m_tile.dispatchAsGroupCount, 1, 1);

    return cl;
}
