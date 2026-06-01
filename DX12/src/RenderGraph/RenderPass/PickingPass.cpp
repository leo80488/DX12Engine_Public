#include "RenderGraph/RenderPass/PickingPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

// Root parameter slot constants (match GraphicsDX12 root signature layout)
static constexpr uint32_t kInstanceBufSlot = 8;
static constexpr uint32_t kMeshDescSlot    = 9;
static constexpr uint32_t kBindlessSlot    = 14;

// ---------------------------------------------------------------------------
PickingPass::~PickingPass()
{
    if (m_gfx)
    {
        if (m_idTexture.IsValid())      m_gfx->DestroyTexture(m_idTexture);
        if (m_depthTexture.IsValid())   m_gfx->DestroyTexture(m_depthTexture);
        if (m_readbackBuffer.IsValid()) m_gfx->DestroyBuffer(m_readbackBuffer);
    }
}

void PickingPass::Setup(RG::RenderGraphBuilder&) {}   // no graph-managed textures

// ---------------------------------------------------------------------------
void PickingPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // Free resources from a previous Init (graph recompile on window resize).
    if (m_readbackBuffer.IsValid()) gfx.DestroyBuffer(m_readbackBuffer);
    if (m_idTexture.IsValid())      gfx.DestroyTexture(m_idTexture);
    if (m_depthTexture.IsValid())   gfx.DestroyTexture(m_depthTexture);
    m_texWidth = m_texHeight = 0;       // force EnsureTextures to recreate
    m_pickRequested = m_pickPending = false;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::PickingID_VS, RHI::ShaderStage::VS, "PickingID.vs.hlsl");
    m_shaderLib.Register(ShaderID::PickingID_PS, RHI::ShaderStage::PS, "PickingID.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    if (!m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_ERROR("PickingPass: PSO creation failed");
    else
        LOG_INFO("PickingPass: PSO ready");

    // Create single-pixel READBACK buffer (size = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT).
    RHI::GPUBufferDesc readbackDesc;
    readbackDesc.size       = 512;
    readbackDesc.usage      = RHI::Usage::READBACK;
    readbackDesc.bind_flags = RHI::BindFlag::NONE;
    if (!gfx.CreateBuffer(readbackDesc, m_readbackBuffer))
        LOG_ERROR("PickingPass: readback buffer creation failed");
    else
        LOG_INFO("PickingPass: readback buffer created");
}

// ---------------------------------------------------------------------------
// OnCompile — called serially after graph textures are (re)created.
// Destroys own textures and pre-creates them at the current render size so
// that EnsureTextures in Execute is always a no-op during the parallel phase.
// ---------------------------------------------------------------------------
void PickingPass::OnCompile(IGraphicsDevice& gfx)
{
    if (m_idTexture.IsValid())    { gfx.DestroyTexture(m_idTexture);    m_idTexture    = {}; }
    if (m_depthTexture.IsValid()) { gfx.DestroyTexture(m_depthTexture); m_depthTexture = {}; }
    m_texWidth  = 0;
    m_texHeight = 0;
    m_idState   = RHI::ResourceState::RENDERTARGET;
    m_pickRequested = false;
    m_pickPending   = false;

    const uint32_t w = gfx.GetRenderWidth();
    const uint32_t h = gfx.GetRenderHeight();
    if (w > 0 && h > 0)
        EnsureTextures(gfx, w, h);
}

// ---------------------------------------------------------------------------
PSODesc PickingPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::PickingID_VS;
    desc.psID        = ShaderID::PickingID_PS;
    desc.inputLayout = InputLayoutType::None;   // PVF

    // No back-face culling: double-sided transparents (leaves, banners, decals)
    // must be pickable from either side. PickingPass only executes on click
    // frames (see Execute() early-out), so the extra raster cost is negligible.
    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = true;

    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ALL;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.bs.render_target[0].render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R32_UINT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT;
    return desc;
}

// ---------------------------------------------------------------------------
void PickingPass::EnsureTextures(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
{
    if (w == m_texWidth && h == m_texHeight) return;

    if (m_idTexture.IsValid())    gfx.DestroyTexture(m_idTexture);
    if (m_depthTexture.IsValid()) gfx.DestroyTexture(m_depthTexture);

    // ---- R32_UINT ID texture -----------------------------------------------
    {
        RHI::TextureDesc desc;
        desc.width      = w;
        desc.height     = h;
        desc.format     = RHI::Format::R32_UINT;
        desc.bind_flags = RHI::BindFlag::RENDER_TARGET;
        desc.layout     = RHI::ResourceState::RENDERTARGET;
        desc.clear.color[0] = desc.clear.color[1] = desc.clear.color[2] = desc.clear.color[3] = 0.f;
        desc.debug_name = "PickingPass.IDTexture";
        if (!gfx.CreateTexture(desc, m_idTexture))
            LOG_ERROR("PickingPass: ID texture creation failed");
        m_idState = RHI::ResourceState::RENDERTARGET;
    }

    // ---- D32_FLOAT depth buffer --------------------------------------------
    {
        RHI::TextureDesc desc;
        desc.width      = w;
        desc.height     = h;
        desc.format     = RHI::Format::D32_FLOAT;
        desc.bind_flags = RHI::BindFlag::DEPTH_STENCIL;
        desc.layout     = RHI::ResourceState::DEPTHSTENCIL;
        desc.clear.depth_stencil.depth   = 0.f;  // reversed Z
        desc.clear.depth_stencil.stencil = 0;
        desc.debug_name = "PickingPass.Depth";
        if (!gfx.CreateTexture(desc, m_depthTexture))
            LOG_ERROR("PickingPass: depth texture creation failed");
    }

    m_texWidth  = w;
    m_texHeight = h;
    LOG_INFO("PickingPass: resized textures to %ux%u", w, h);
}

// ---------------------------------------------------------------------------
void PickingPass::RequestPick(int pixelX, int pixelY)
{
    m_pickX         = pixelX;
    m_pickY         = pixelY;
    m_pickRequested = true;
    // m_pickPending is NOT set here — it is set by Execute() once the GPU copy is recorded.
    // This prevents ResolvePick() from reading stale data on the same frame as the click.
}

// ---------------------------------------------------------------------------
uint32_t PickingPass::ReadPickResult() const
{
    if (!m_readbackBuffer.IsValid() || !m_gfx) return 0;

    void* pData = m_gfx->MapBuffer(m_readbackBuffer);
    if (!pData) return 0;
    const uint32_t result = *static_cast<const uint32_t*>(pData);
	LOG_INFO("Picking POS = ( %u , %u )", m_pickX,m_pickY);
	LOG_INFO("PickingPass: picked instance slot = %u", result);
    m_gfx->UnmapBuffer(m_readbackBuffer);
    return result;
}

// ---------------------------------------------------------------------------
RHI::CommandList PickingPass::Execute(RHI::CommandList cl)
{
    // Picking is request-driven: unless a RequestPick(x, y) is outstanding
    // (m_pickRequested) or a previous frame's copy is still in flight
    // awaiting readback (m_pickPending), there is nothing to do. Skipping
    // here avoids a full opaque draw-list render + texture clears + render
    // target binds every frame — the pass was consuming several hundred
    // μs per frame even when no one had clicked the viewport.
    if (!m_pickRequested && !m_pickPending)
        return cl;

    const uint32_t vpW = cl.GetWidth();
    const uint32_t vpH = cl.GetHeight();
    if (vpW == 0 || vpH == 0) return cl;

    EnsureTextures(cl.GetDevice(), vpW, vpH);
    if (!m_idTexture.IsValid() || !m_depthTexture.IsValid()) return cl;

    // ---- Transition ID texture to RENDER_TARGET if it's in COPY_SRC --------
    if (m_idState == RHI::ResourceState::COPY_SRC)
    {
        cl.PushBarrier(RHI::GPUBarrier::Image(&m_idTexture,
            RHI::ResourceState::COPY_SRC, RHI::ResourceState::RENDERTARGET));
        m_idState = RHI::ResourceState::RENDERTARGET;
    }

    // ---- Bind and clear ----------------------------------------------------
    const RHI::Texture* rtvs[] = { &m_idTexture };
    cl.SetRenderTargets(1, rtvs, &m_depthTexture);

    const float clearZero[4] = { 0.f, 0.f, 0.f, 0.f };
    cl.ClearRenderTarget(m_idTexture, clearZero);
    cl.ClearDepthStencil(m_depthTexture, 0.f, 0);  // reversed Z

    cl.SetViewport(vpW, vpH);
    cl.SetScissorRect(vpW, vpH);
    cl.SetPrimitiveTopology();

    // ---- Draw opaque + transparent packets ---------------------------------
    // Transparent meshes share the same PSO (R32_UINT RTV, reverse-Z depth
    // GREATER_EQUAL). Drawing them after opaques into our private depth buffer
    // makes the frontmost-fragment win — same selection rule editors use
    // (Blender / Unity / Unreal): clicking a glass cube picks the glass,
    // regardless of its alpha.
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc());
    if (!pso || !pso->IsValid())
        return cl;

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();

    cl.SetPipelineState(*pso);
    cl.BindDescriptorHeaps();
    cl.BindBufferSRVByName(kInstanceBufSlot, "InstanceBuffer");
    cl.BindBufferSRVByName(kMeshDescSlot,    "MeshDescriptors");
    if (bindlessHandle)
        cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
    cl.BindCBByName(0, "PerView");

    auto drawList = [&](DrawFilter f)
    {
        for (const DrawPacket& dp : cl.GetContext().GetDrawList(f))
        {
            cl.SetPVFRootConstants(dp.meshDescriptorIndex,
                                   dp.instanceOffset,
                                   dp.materialIndex);
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }
    };
    drawList(DrawFilter::Opaque);
    drawList(DrawFilter::Transparent);

    // ---- Copy picked pixel to readback buffer (only if requested this frame) --
    if (m_pickRequested
        && m_pickX >= 0 && m_pickX < static_cast<int>(vpW)
        && m_pickY >= 0 && m_pickY < static_cast<int>(vpH))
    {
        cl.PushBarrier(RHI::GPUBarrier::Image(&m_idTexture,
            RHI::ResourceState::RENDERTARGET, RHI::ResourceState::COPY_SRC));
        m_idState = RHI::ResourceState::COPY_SRC;

        cl.CopyTexturePixelToBuffer(m_idTexture,
            static_cast<uint32_t>(m_pickX),
            static_cast<uint32_t>(m_pickY),
            m_readbackBuffer);

        m_pickRequested = false;
        m_pickPending   = true;
    }

    return cl;
}
