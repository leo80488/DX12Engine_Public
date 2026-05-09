#include "RenderGraph/RenderPass/TracerPasses.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/TracerSystem.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cstring>

// Compute root signature slots (must match GraphicsDX12::CreateComputeRootSignature).
// Same physical slots ParticleSimPass uses; we just bind a different SRV at
// slot 1 (gSpawns instead of restPositions).
static constexpr uint32_t kCSCBSlot       = 0;  // b0 space2 (ROOT_CBV)
static constexpr uint32_t kCSSpawnsSlot   = 1;  // DESC_TABLE t0 space2 (spawn upload SRV)
static constexpr uint32_t kCSPoolUAVSlot  = 4;  // DESC_TABLE u0 space2 (pool UAV)

// Graphics root signature slots (must match CreatePVFRootSignature).
static constexpr uint32_t kGfxRenderCBSlot   = 2;  // ROOT_CBV b2 space0 (BindConstantBuffer slot 1)
static constexpr uint32_t kGfxPoolSRVSlot    = 12; // DESC_TABLE t4 space0
static constexpr uint32_t kGfxDepthSRVSlot   = 13; // DESC_TABLE t5 space0
static constexpr uint32_t kGfxBindlessTexSlot= 27; // DESC_TABLE t0 space2 — bindless texture array
static constexpr uint32_t kGfxSamplerSlot    = 15; // s0 space0 — linear sampler (BindSampler slot 0)

// ===========================================================================
// TracerSimPass
// ===========================================================================

void TracerSimPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::TracerEmit_CS,   RHI::ShaderStage::CS, "TracerEmit.cs.hlsl",   "CSMain");
    m_shaderLib.Register(ShaderID::TracerUpdate_CS, RHI::ShaderStage::CS, "TracerUpdate.cs.hlsl", "CSMain");

    const RHI::Shader* emitCS   = m_shaderLib.GetShader(ShaderID::TracerEmit_CS);
    const RHI::Shader* updateCS = m_shaderLib.GetShader(ShaderID::TracerUpdate_CS);
    if (!emitCS || !updateCS)
    {
        LOG_ERROR("TracerSimPass: compute shader lookup failed");
        return;
    }

    {
        RHI::PipelineStateDesc d{};
        d.cs = emitCS;
        if (!gfx.CreatePipelineState(d, m_emitPSO))
            LOG_ERROR("TracerSimPass: emit PSO failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = updateCS;
        if (!gfx.CreatePipelineState(d, m_updatePSO))
            LOG_ERROR("TracerSimPass: update PSO failed");
    }

    LOG_SUCCESS("TracerSimPass: initialised");
}

void TracerSimPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_emitPSO.IsValid() || !m_updatePSO.IsValid()) return;
    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // ---- Phase A: Emit (single dispatch over all spawns this frame) ----
    const uint32_t spawnCount = m_sys->GetSpawnCount();
    if (spawnCount > 0)
    {
        gfx.BindComputePipelineState(m_emitPSO, cl);
        gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetSystemCB(), 0, cl);
        gfx.SetComputeDescriptorTable(kCSSpawnsSlot,  m_sys->GetSpawnSRV(), cl);
        gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, m_sys->GetPoolUAV(), cl);

        gfx.DispatchCompute((spawnCount + 63) / 64, 1, 1, cl);

        // Emit writes must complete before update reads.
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetPool()), cl);
    }

    // ---- Phase B: Update (one dispatch over the entire pool) -----------
    gfx.BindComputePipelineState(m_updatePSO, cl);
    gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetSystemCB(), 0, cl);
    gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, m_sys->GetPoolUAV(), cl);

    const uint32_t poolSize = TracerSystem::GetPoolCapacity();
    gfx.DispatchCompute((poolSize + 63) / 64, 1, 1, cl);

    // Render pass needs the pool in a stable state.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetPool()), cl);
}

// ===========================================================================
// TracerRenderPass
// ===========================================================================

TracerRenderPass::TracerRenderPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

void TracerRenderPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth is sampled in PS for soft-particle fade. Same convention as
    // OutlinePass: don't declare ReadSRV here — handle the
    // DEPTHSTENCIL→SHADER_RESOURCE transition manually inside Execute and
    // restore at the end. Declaring it here would double-transition vs. the
    // manual barrier and trip a state-tracking assert.
    // HDR RTV is bound manually in Execute, same as TransparentPass.
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void TracerRenderPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Tracer_VS, RHI::ShaderStage::VS, "Tracer.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::Tracer_PS, RHI::ShaderStage::PS, "Tracer.ps.hlsl", "main");

    const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::Tracer_VS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Tracer_PS);
    if (!vs || !ps)
    {
        LOG_ERROR("TracerRenderPass: shader lookup failed");
        return;
    }

    // PSO: triangle strip 4 verts/instance, additive premultiplied via SRC_ALPHA,
    // depth test OFF (we sample depth in PS via SRV instead), depth write OFF.
    RHI::RasterizerState rs{};
    rs.fill_mode         = RHI::FillMode::SOLID;
    rs.cull_mode         = RHI::CullMode::NONE;       // double-sided
    rs.depth_clip_enable = true;

    RHI::DepthStencilState dss{};
    dss.depth_enable     = false;                     // depth handled in PS
    dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    dss.stencil_enable   = false;

    RHI::BlendState bs{};
    auto& rt = bs.render_target[0];
    rt.blend_enable             = true;
    rt.src_blend                = RHI::Blend::SRC_ALPHA;
    rt.dest_blend               = RHI::Blend::ONE;
    rt.blend_op                 = RHI::BlendOp::ADD;
    rt.src_blend_alpha          = RHI::Blend::ONE;
    rt.dest_blend_alpha         = RHI::Blend::ONE;
    rt.blend_op_alpha           = RHI::BlendOp::ADD;
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    RHI::PipelineStateDesc d{};
    d.vs             = vs;
    d.ps             = ps;
    d.rs             = &rs;
    d.dss            = &dss;
    d.bs             = &bs;
    d.pt             = RHI::PrimitiveTopology::TRIANGLESTRIP;
    d.rtv_formats[0] = RHI::Format::R16G16B16A16_FLOAT;
    d.rtv_count      = 1;
    d.dsv_format     = RHI::Format::UNKNOWN;          // no DSV
    d.sample_count   = 1;

    if (!gfx.CreatePipelineState(d, m_pso))
    {
        LOG_ERROR("TracerRenderPass: PSO creation failed");
        return;
    }

    // Per-frame render CB.
    {
        RHI::GPUBufferDesc cbd{};
        cbd.size       = 256;
        cbd.usage      = RHI::Usage::UPLOAD;
        cbd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(cbd, m_renderCB))
            m_renderCBMapped = gfx.MapBuffer(m_renderCB);
        if (!m_renderCBMapped)
            LOG_ERROR("TracerRenderPass: render CB map failed");
    }

    // Linear-wrap sampler for the optional noise texture.
    RHI::SamplerDesc sd;
    if (!gfx.CreateSampler(sd, m_linearSamplerIdx))
        LOG_ERROR("TracerRenderPass: sampler creation failed");

    LOG_SUCCESS("TracerRenderPass: initialised");
}

RHI::CommandList TracerRenderPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_pso.IsValid() || !m_renderCBMapped) return cl;

    using namespace DirectX;

    // Pack the render CB. Matrix transposed for HLSL row-vector mul().
    RenderCB cb{};
    XMMATRIX vp = XMLoadFloat4x4(&m_viewProj);
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),
                    XMMatrixTranspose(vp));
    cb.cameraPos[0]  = m_cameraPos.x;
    cb.cameraPos[1]  = m_cameraPos.y;
    cb.cameraPos[2]  = m_cameraPos.z;
    cb.time          = m_time;
    cb.nearZ         = m_nearZ;
    cb.farZ          = m_farZ;
    cb.fadeRange     = m_fadeRange;
    cb.coreSharpness = m_coreSharpness;
    cb.noiseTiling   = m_noiseTiling;
    cb.scrollSpeed   = m_scrollSpeed;
    cb.noiseFloor    = m_noiseFloor;
    std::memcpy(m_renderCBMapped, &cb, sizeof(cb));

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Manual depth state transition: DEPTHSTENCIL → SHADER_RESOURCE so the PS
    // can sample it. Restored at the end of the pass for downstream
    // consumers (Outline restores; we do the same).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (!depthTex) return cl;

    cl.PushBarrier(RHI::GPUBarrier::Image(
        depthTex,
        RHI::ResourceState::DEPTHSTENCIL,
        RHI::ResourceState::SHADER_RESOURCE));

    // Bind HDR RTV with NO DSV — we depth-compare in PS.
    gfx.SetRenderTargetToHdrWithDepth(nullptr, cl);
    cl.SetViewport();
    cl.SetScissorRect();

    cl.SetPipelineState(m_pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLESTRIP);

    // Render CB at b2 space0.
    gfx.BindConstantBuffer(m_renderCB, /*slot=*/1, cl);

    // Pool SRV at t4 space0.
    cl.BindDescriptorTableHandle(kGfxPoolSRVSlot, m_sys->GetPoolSRV());

    // Scene depth SRV at t5 space0.
    const uint64_t depthSrv = gfx.GetTextureSRVGpuHandle(*depthTex);
    if (depthSrv)
        cl.BindDescriptorTableHandle(kGfxDepthSRVSlot, depthSrv);

    // Bindless texture table — Tracer.ps.hlsl optionally indexes
    // g_AllTextures[noiseIdx] when a tracer carries a non-sentinel noise idx.
    D3D12_GPU_DESCRIPTOR_HANDLE texTable = gfx.GetBindlessTextureTableHandle();
    if (texTable.ptr)
        cl.BindDescriptorTableHandle(kGfxBindlessTexSlot, texTable.ptr);

    if (m_linearSamplerIdx >= 0)
        cl.BindSampler(0, m_linearSamplerIdx);

    // 4 verts (TRIANGLESTRIP) per instance, one instance per pool slot.
    // Dead slots emit a degenerate quad in VS so the rasteriser skips them.
    gfx.DrawInstanced(4, TracerSystem::GetPoolCapacity(), 0, 0, cl);

    // Restore depth state for downstream passes (TAA expects DEPTHSTENCIL).
    cl.PushBarrier(RHI::GPUBarrier::Image(
        depthTex,
        RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::DEPTHSTENCIL));

    return cl;
}
