#include "RenderGraph/RenderPass/VideoQuadPass.h"
#include "ECS/ECS.h"
#include "ECS/VideoComponent.h"
#include "ECS/HierarchyComponents.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>

using namespace DirectX;

// Same root-sig slots as the screen-space VideoPass:
//   t6 space0 → root slot 19 (Y plane)
//   t7 space0 → root slot 20 (UV plane)
//   b2 space0 → BindConstantBufferAtOffset(slot=1)
static constexpr uint32_t kYPlaneRootSlot  = 19;
static constexpr uint32_t kUVPlaneRootSlot = 20;
static constexpr uint32_t kCBSlot          = 1;   // b2 via root[2]

VideoQuadPass::VideoQuadPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{
    XMStoreFloat4x4(&m_viewProj, XMMatrixIdentity());
}

VideoQuadPass::~VideoQuadPass()
{
    if (m_gfx)
    {
        for (auto& ring : m_cbRing)
        {
            if (ring.mapped) m_gfx->UnmapBuffer(ring.buffer);
            if (ring.buffer.IsValid()) m_gfx->DestroyBuffer(ring.buffer);
            ring.mapped = nullptr;
            ring.capacitySlots = 0;
        }
    }
}

// ---------------------------------------------------------------------------
void VideoQuadPass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_depth);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// ---------------------------------------------------------------------------
void VideoQuadPass::EnsureCBRing(IGraphicsDevice& gfx, uint32_t frameIdx, uint32_t slots)
{
    if (frameIdx >= kFrameCount) return;
    auto& ring = m_cbRing[frameIdx];
    if (ring.buffer.IsValid() && ring.capacitySlots >= slots) return;

    // Free the old one (deferred-release).
    if (ring.mapped) { gfx.UnmapBuffer(ring.buffer); ring.mapped = nullptr; }
    if (ring.buffer.IsValid()) gfx.DestroyBuffer(ring.buffer);

    RHI::GPUBufferDesc bd{};
    bd.size       = static_cast<uint64_t>(slots) * kCBStride;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (!gfx.CreateBuffer(bd, ring.buffer))
    {
        LOG_ERROR("VideoQuadPass: CB ring buffer create failed (slots=%u)", slots);
        ring.capacitySlots = 0;
        return;
    }
    ring.mapped = static_cast<uint8_t*>(gfx.MapBuffer(ring.buffer));
    ring.capacitySlots = slots;
    LOG_INFO("VideoQuadPass: CB ring slot %u sized for %u quads (%llu B)",
             frameIdx, slots, bd.size);
}

// ---------------------------------------------------------------------------
void VideoQuadPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VideoQuad_VS, RHI::ShaderStage::VS,
                         "VideoQuad.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::VideoQuad_PS, RHI::ShaderStage::PS,
                         "VideoQuad.ps.hlsl", "main");

    m_psoCache.Init(gfx, m_shaderLib);
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VideoQuad_VS;
        desc.psID        = ShaderID::VideoQuad_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cull_mode         = RHI::CullMode::NONE;
        desc.rs.depth_clip_enable = true;
        desc.dss.depth_enable     = true;
        desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
        desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;
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
        desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

        if (m_psoCache.GetOrCreate(desc))
            m_psoCreated = true;
        else
            LOG_ERROR("VideoQuadPass: PSO create failed");
    }

    // Initial CB rings — 16 slots × 256 B = 4 KB per frame buffer.
    for (uint32_t i = 0; i < kFrameCount; ++i)
        EnsureCBRing(gfx, i, kCBInitialSlots);

    RHI::SamplerDesc sd;
    sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
    sd.address_u = RHI::TextureAddressMode::CLAMP;
    sd.address_v = RHI::TextureAddressMode::CLAMP;
    sd.address_w = RHI::TextureAddressMode::CLAMP;
    gfx.CreateSampler(sd, m_linearSamplerSlot);

    LOG_SUCCESS("VideoQuadPass: initialised");
}

void VideoQuadPass::SetCamera(const XMFLOAT4X4& viewProjNoJitter)
{
    m_viewProj = viewProjNoJitter;
}

// ---------------------------------------------------------------------------
RHI::CommandList VideoQuadPass::Execute(RHI::CommandList cl)
{
    if (!m_gfx || !m_psoCreated || !m_world) return cl;

    IGraphicsDevice& gfx = *m_gfx;
    const uint32_t   frameIdx = gfx.GetFrameIndex() % kFrameCount;

    // Honour any growth request from the previous frame BEFORE we start
    // writing into this slot. The deferred-release of the old buffer is safe
    // because GraphicsDX12 holds it until its frame-slot fence retires.
    if (m_pendingGrowSlots > 0)
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
            EnsureCBRing(gfx, i, m_pendingGrowSlots);
        m_pendingGrowSlots = 0;
    }

    auto& ring = m_cbRing[frameIdx];
    if (!ring.mapped || ring.capacitySlots == 0) return cl;

    // ---- Collect every active world-space VideoComponent first -----------
    struct Active { Entity e; const VideoComponent* vc; };
    std::vector<Active> actives;
    actives.reserve(8);
    m_world->ForEach<VideoComponent>(
        [&](Entity e, const VideoComponent& vc)
    {
        if (!vc.worldSpace) return;
        if (vc.state == VideoPlaybackState::Stopped) return;
        if (vc.currentDpbSlot >= vc.dpb.size()) return;
        if (!vc.dpb[vc.currentDpbSlot].IsValid()) return;
        actives.push_back({ e, &vc });
    });
    if (actives.empty()) return cl;

    // Schedule a grow for next frame if today's batch exceeds capacity —
    // anything past capacity gets dropped THIS frame (with a warning) so we
    // never overrun the mapped buffer; next frame's ring is sized to fit.
    if (actives.size() > ring.capacitySlots)
    {
        m_pendingGrowSlots = (std::max)(uint32_t(actives.size()),
                                        ring.capacitySlots * 2);
        LOG_WARNING("VideoQuadPass: %zu world-space videos but ring holds %u "
                    "— dropping %zu this frame, growing to %u next frame",
                    actives.size(), ring.capacitySlots,
                    actives.size() - ring.capacitySlots, m_pendingGrowSlots);
        actives.resize(ring.capacitySlots);
    }

    // ---- Bind common state -----------------------------------------------
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    gfx.SetRenderTargetToHdrWithDepth(depthTex, cl);
    cl.BindDescriptorHeaps();

    PSODesc desc;
    desc.vsID        = ShaderID::VideoQuad_VS;
    desc.psID        = ShaderID::VideoQuad_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = true;
    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;
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
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return cl;

    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);
    if (m_linearSamplerSlot >= 0) cl.BindSampler(0, m_linearSamplerSlot);

    auto& dx12 = static_cast<GraphicsDX12&>(gfx);

    // ---- Per-quad: write CB into ring, bind, draw ------------------------
    for (uint32_t i = 0; i < actives.size(); ++i)
    {
        const Active& a = actives[i];
        const RHI::Texture& nv12 = a.vc->dpb[a.vc->currentDpbSlot];

        XMFLOAT4X4 world;
        if (auto* gt = m_world->GetComponent<GlobalTransform>(a.e))
            world = gt->matrix;
        else
            XMStoreFloat4x4(&world, XMMatrixIdentity());

        // Write CB into ring at offset i * kCBStride.
        VideoQuadCB c{};
        std::memcpy(c.worldMatrix,    &world,      sizeof(c.worldMatrix));
        std::memcpy(c.viewProjMatrix, &m_viewProj, sizeof(c.viewProjMatrix));
        c.quadWidth  = a.vc->worldWidth;
        c.quadHeight = a.vc->worldHeight;
        c.alpha      = a.vc->renderAlpha;
        c.colorSpace = a.vc->colorSpace;
        std::memcpy(ring.mapped + i * kCBStride, &c, sizeof(c));

        dx12.BindConstantBufferAtOffset(kCBSlot, ring.buffer,
                                        uint64_t(i) * kCBStride, cl);
        cl.BindDescriptorTableHandle(kYPlaneRootSlot,
            gfx.GetTextureSRVGpuHandle(nv12));
        cl.BindDescriptorTableHandle(kUVPlaneRootSlot,
            gfx.GetTextureUVPlaneSRVGpuHandle(nv12));
        cl.DrawInstanced(6, 1, 0, 0);
    }

    return cl;
}
