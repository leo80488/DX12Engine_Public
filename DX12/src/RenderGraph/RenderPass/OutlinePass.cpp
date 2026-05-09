#include "RenderGraph/RenderPass/OutlinePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>

// ---- Root parameter slot indices (graphics root sig, match GraphicsDX12.cpp) ---
static constexpr uint32_t kPerViewCBSlot   = 0;  // BindCBByName slot 0 → b1 space0
static constexpr uint32_t kOutlineCBSlot   = 1;  // BindConstantBuffer slot 1 → b2 space0
static constexpr uint32_t kInstanceBufSlot = 8;
static constexpr uint32_t kMeshDescSlot    = 9;
static constexpr uint32_t kBindlessSlot    = 14;

// SRV descriptor table slots used by the screen-space PS.
// Maps to root params [10..13] → t2..t5 space0.
static constexpr uint32_t kObjectIDSrvSlot = 10; // t2 space0
static constexpr uint32_t kNormalSrvSlot   = 11; // t3 space0
static constexpr uint32_t kDepthSrvSlot    = 13; // t5 space0

// ---------------------------------------------------------------------------
OutlinePass::OutlinePass(RG::RGTextureHandle depth, RG::RGTextureHandle normal)
    : m_depth(depth), m_normal(normal)
{}

// ---------------------------------------------------------------------------
OutlinePass::~OutlinePass()
{
    if (!m_gfxPtr) return;
    if (m_outlineCBMapped) m_gfxPtr->UnmapBuffer(m_outlineCB);
    if (m_objectIdTex.IsValid()) m_gfxPtr->DestroyTexture(m_objectIdTex);
}

// ---------------------------------------------------------------------------
void OutlinePass::Setup(RG::RenderGraphBuilder& b)
{
    // Keep normal in SHADER_RESOURCE so sub-pass 3 can sample it.
    b.ReadSRV(m_normal);
    // HDR and depth are managed manually in Execute().
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// ---------------------------------------------------------------------------
void OutlinePass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    // ---- OutlineCB ---------------------------------------------------------
    if (m_outlineCBMapped) { gfx.UnmapBuffer(m_outlineCB); m_outlineCBMapped = nullptr; }
    {
        RHI::GPUBufferDesc bd;
        bd.size       = (sizeof(OutlineCBData) + 255u) & ~255u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_outlineCB))
            m_outlineCBMapped = gfx.MapBuffer(m_outlineCB);
        else
            LOG_ERROR("OutlinePass: OutlineCB creation failed");
    }

    // ---- ObjectID texture (matches current render resolution) --------------
    const uint32_t w = gfx.GetRenderWidth();
    const uint32_t h = gfx.GetRenderHeight();
    RebuildObjectIdTexture(gfx, w, h);

    // ---- Shaders -----------------------------------------------------------
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS,            RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    m_shaderLib.Register(ShaderID::Lighting_VS,           RHI::ShaderStage::VS, "Lighting.vs.hlsl");
    m_shaderLib.Register(ShaderID::OutlineHull_VS,        RHI::ShaderStage::VS, "InvertedHull.vs.hlsl");
    m_shaderLib.Register(ShaderID::OutlineHull_PS,        RHI::ShaderStage::PS, "InvertedHull.ps.hlsl");
    m_shaderLib.Register(ShaderID::OutlineObjectID_PS,    RHI::ShaderStage::PS, "OutlineObjectID.ps.hlsl");
    m_shaderLib.Register(ShaderID::OutlineScreenSpace_PS, RHI::ShaderStage::PS, "OutlineScreenSpace.ps.hlsl");

    // ---- PSO caches --------------------------------------------------------
    m_hullPsoCache.Init(gfx, m_shaderLib);
    m_objectIdPsoCache.Init(gfx, m_shaderLib);
    m_screenSpacePsoCache.Init(gfx, m_shaderLib);

    if (!m_hullPsoCache.GetOrCreate(BuildHullPSODesc()))
        LOG_ERROR("OutlinePass: hull PSO creation failed");
    else
        LOG_INFO("OutlinePass: hull PSO ready");

    if (!m_objectIdPsoCache.GetOrCreate(BuildObjectIdPSODesc()))
        LOG_ERROR("OutlinePass: objectId PSO creation failed");
    else
        LOG_INFO("OutlinePass: objectId PSO ready");

    if (!m_screenSpacePsoCache.GetOrCreate(BuildScreenSpacePSODesc()))
        LOG_ERROR("OutlinePass: screen-space PSO creation failed");
    else
        LOG_SUCCESS("OutlinePass: initialized (%ux%u)", w, h);
}

// ---------------------------------------------------------------------------
void OutlinePass::RebuildObjectIdTexture(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
{
    if (m_objectIdTex.IsValid()) gfx.DestroyTexture(m_objectIdTex);

    m_objectIdW     = 0;
    m_objectIdH     = 0;
    m_objectIdState = RHI::ResourceState::UNDEFINED;

    if (w == 0 || h == 0) return;

    RHI::TextureDesc td;
    td.width      = w;
    td.height     = h;
    td.format     = RHI::Format::R32_UINT;
    td.bind_flags = RHI::BindFlag::RENDER_TARGET | RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.mip_levels = 1;
    td.layout     = RHI::ResourceState::RENDERTARGET;  // start directly as RTV
    td.debug_name = "OutlinePass.ObjectID";

    if (!gfx.CreateTexture(td, m_objectIdTex))
    {
        LOG_ERROR("OutlinePass: ObjectID texture creation failed (%ux%u)", w, h);
        return;
    }
    m_objectIdState = RHI::ResourceState::RENDERTARGET;
    m_objectIdW = w;
    m_objectIdH = h;
}

// ---------------------------------------------------------------------------
PSODesc OutlinePass::BuildHullPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::OutlineHull_VS;
    desc.psID        = ShaderID::OutlineHull_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cull_mode         = RHI::CullMode::FRONT;
    desc.rs.depth_clip_enable = true;

    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    // Alpha blending: outline fades with distance to prevent dense-mesh
    // areas (hair) from becoming solid black at a distance.
    auto& rt0 = desc.bs.render_target[0];
    rt0.blend_enable           = true;
    rt0.src_blend              = RHI::Blend::SRC_ALPHA;
    rt0.dest_blend             = RHI::Blend::INV_SRC_ALPHA;
    rt0.blend_op               = RHI::BlendOp::ADD;
    rt0.src_blend_alpha        = RHI::Blend::ONE;
    rt0.dest_blend_alpha       = RHI::Blend::ZERO;
    rt0.blend_op_alpha         = RHI::BlendOp::ADD;
    rt0.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;
    return desc;
}

// ---------------------------------------------------------------------------
PSODesc OutlinePass::BuildObjectIdPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::GBuffer_VS;
    desc.psID        = ShaderID::OutlineObjectID_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cull_mode         = RHI::CullMode::BACK;
    desc.rs.depth_clip_enable = true;

    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;  // GBuffer already wrote depth
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.rtvFormats[0] = RHI::Format::R32_UINT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;
    return desc;
}

// ---------------------------------------------------------------------------
PSODesc OutlinePass::BuildScreenSpacePSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::Lighting_VS;
    desc.psID        = ShaderID::OutlineScreenSpace_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = false;

    desc.dss.depth_enable     = false;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;

    auto& rt = desc.bs.render_target[0];
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    rt.blend_enable             = true;
    rt.src_blend                = RHI::Blend::SRC_ALPHA;
    rt.dest_blend               = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op                 = RHI::BlendOp::ADD;
    rt.src_blend_alpha          = RHI::Blend::ONE;
    rt.dest_blend_alpha         = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op_alpha           = RHI::BlendOp::ADD;

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;
    return desc;
}

// ---------------------------------------------------------------------------
RHI::CommandList OutlinePass::Execute(RHI::CommandList cl)
{
    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Custom);
    if (draws.empty()) return cl;

    if (!m_objectIdTex.IsValid()) return cl;

    const RHI::PipelineState* hullPso = m_hullPsoCache.GetOrCreate(BuildHullPSODesc());
    const RHI::PipelineState* objIdPso = m_objectIdPsoCache.GetOrCreate(BuildObjectIdPSODesc());
    const RHI::PipelineState* ssPso = m_screenSpacePsoCache.GetOrCreate(BuildScreenSpacePSODesc());
    if (!hullPso || !objIdPso || !ssPso) return cl;

    IGraphicsDevice& gfx = *m_gfxPtr;

    const RHI::Texture* depthTex  = cl.GetContext().GetTexture(m_depth);
    const RHI::Texture* normalTex = cl.GetContext().GetTexture(m_normal);
    if (!depthTex || !normalTex) return cl;

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const uint32_t renderW = gfx.GetRenderWidth();
    const uint32_t renderH = gfx.GetRenderHeight();

    if (renderW != m_objectIdW || renderH != m_objectIdH)
        RebuildObjectIdTexture(gfx, renderW, renderH);
    if (!m_objectIdTex.IsValid()) return cl;

    // ---- Upload OutlineCB --------------------------------------------------
    if (m_outlineCBMapped)
    {
        OutlineCBData cb;
        cb.outlinePixels   = outlinePixels;
        cb.depthThreshold  = depthThreshold;
        cb.normalThreshold = normalThreshold;
        cb.outlineStrength = outlineStrength;
        cb.outlineColor[0] = outlineColor[0];
        cb.outlineColor[1] = outlineColor[1];
        cb.outlineColor[2] = outlineColor[2];
        cb.outlineFadeStart = outlineFadeStart;
        cb.vpWidth          = renderW;
        cb.vpHeight         = renderH;
        cb.outlineFadeEnd   = outlineFadeEnd;
        cb.nearZ = nearZ;
        cb.farZ  = farZ;
        std::memcpy(m_outlineCBMapped, &cb, sizeof(cb));
    }

    auto bindGeometryGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindBufferSRVByName(kInstanceBufSlot, "InstanceBuffer");
        cl.BindBufferSRVByName(kMeshDescSlot,    "MeshDescriptors");
        if (bindlessHandle)
            cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
        cl.BindCBByName(kPerViewCBSlot, "PerView");
    };

    // =====================================================================
    // Sub-pass 1: Inverted Hull → HDR RTV + depth DSV (read-only test)
    // HDR starts in RENDER_TARGET; depth starts in DEPTHSTENCIL.
    // =====================================================================
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl);
    cl.SetViewport(renderW, renderH);
    cl.SetScissorRect(renderW, renderH);
    cl.SetPrimitiveTopology();

    cl.SetPipelineState(*hullPso);
    bindGeometryGlobals();
    cl.GetDevice().BindConstantBuffer(m_outlineCB, kOutlineCBSlot, cl);

    for (const DrawPacket& dp : draws)
    {
        cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
        // Set outlinePixels as the 4th root constant (word offset 3) per draw.
        gfx.SetGraphicsRootConstant(
            0, *reinterpret_cast<const uint32_t*>(&dp.outlinePixels), 3, cl);
        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }

    // =====================================================================
    // Sub-pass 2: Object ID → R32_UINT RTV + depth DSV (depth test + write)
    // Transition ObjectID from its current state → RENDERTARGET.
    // =====================================================================
    if (m_objectIdState != RHI::ResourceState::RENDERTARGET)
    {
        cl.PushBarrier(RHI::GPUBarrier::Image(
            &m_objectIdTex,
            m_objectIdState,
            RHI::ResourceState::RENDERTARGET));
        m_objectIdState = RHI::ResourceState::RENDERTARGET;
    }

    const float zeroClear[4] = {};
    gfx.ClearRenderTarget(m_objectIdTex, zeroClear, cl);
    const RHI::Texture* rts[] = { &m_objectIdTex };
    gfx.SetRenderTargets(1, rts, depthTex, cl);

    cl.SetViewport(renderW, renderH);
    cl.SetScissorRect(renderW, renderH);
    cl.SetPrimitiveTopology();

    cl.SetPipelineState(*objIdPso);
    bindGeometryGlobals();

    for (const DrawPacket& dp : draws)
    {
        if (!dp.screenSpaceOutline) continue; // skip if screen-space outline disabled
        cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }

    // =====================================================================
    // Sub-pass 3: Screen-Space Composite (fullscreen triangle, alpha blend)
    // Transition ObjectID RENDERTARGET → SHADER_RESOURCE.
    // Transition Depth DEPTHSTENCIL → SHADER_RESOURCE (restore afterwards).
    // =====================================================================
    cl.PushBarrier(RHI::GPUBarrier::Image(
        &m_objectIdTex,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE));
    m_objectIdState = RHI::ResourceState::SHADER_RESOURCE;

    cl.PushBarrier(RHI::GPUBarrier::Image(
        depthTex,
        RHI::ResourceState::DEPTHSTENCIL,
        RHI::ResourceState::SHADER_RESOURCE));

    // Bind HDR RTV without depth (nullptr = no DSV).
    cl.GetDevice().SetRenderTargetToHdrWithDepth(nullptr, cl);

    cl.SetPipelineState(*ssPso);
    cl.BindDescriptorHeaps();
    cl.GetDevice().BindConstantBuffer(m_outlineCB, kOutlineCBSlot, cl);

    const uint64_t objIdSrv  = gfx.GetTextureSRVGpuHandle(m_objectIdTex);
    const uint64_t normalSrv = gfx.GetTextureSRVGpuHandle(*normalTex);
    const uint64_t depthSrv  = gfx.GetTextureSRVGpuHandle(*depthTex);

    if (objIdSrv)  cl.BindDescriptorTableHandle(kObjectIDSrvSlot, objIdSrv);
    if (normalSrv) cl.BindDescriptorTableHandle(kNormalSrvSlot,   normalSrv);
    if (depthSrv)  cl.BindDescriptorTableHandle(kDepthSrvSlot,    depthSrv);

    cl.DrawFullscreenTriangle();

    // Restore depth to DEPTHSTENCIL so TAA's hardcoded transition is correct.
    cl.PushBarrier(RHI::GPUBarrier::Image(
        depthTex,
        RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::DEPTHSTENCIL));

    return cl;
}
