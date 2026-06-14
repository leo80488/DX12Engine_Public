#include "RenderGraph/RenderPass/ParticlePasses.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/PSOCache.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cstring>

// Compute root signature slots (must match GraphicsDX12::CreateComputeRootSignature).
// Shared with SkinningPass — see SkinningPass.h for the full table.
static constexpr uint32_t kCSCBSlot        = 0;   // b0 space2 (ROOT_CBV)
static constexpr uint32_t kCSPoolUAVSlot   = 4;   // DESC_TABLE u0 space2
static constexpr uint32_t kCSMeshDescSlot  = 10;  // DESC_TABLE t1 space0 (MeshDescriptors SRV)
static constexpr uint32_t kCSBindlessSlot  = 11;  // DESC_TABLE t0 space1 (bindless g_Buffers[])

// Graphics root signature slots (must match GraphicsDX12::CreatePVFRootSignature).
static constexpr uint32_t kGfxRenderCBSlot   = 2;  // ROOT_CBV b2 space0 (BindConstantBuffer slot 1)
static constexpr uint32_t kGfxPoolSRVSlot    = 12; // DESC_TABLE t4 space0
static constexpr uint32_t kGfxBindlessTexSlot= 27; // DESC_TABLE t0 space2 — bindless texture array
static constexpr uint32_t kGfxSamplerSlot    = 15; // s0 space0 — linear sampler (BindSampler slot 0)

// ===========================================================================
// ParticleSimPass
// ===========================================================================

void ParticleSimPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::ParticleEmit_CS,   RHI::ShaderStage::CS, "ParticleEmit.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::ParticleUpdate_CS, RHI::ShaderStage::CS, "ParticleUpdate.cs.hlsl", "CSMain");

    const RHI::Shader* emitCS   = m_shaderLib.GetShader(ShaderID::ParticleEmit_CS);
    const RHI::Shader* updateCS = m_shaderLib.GetShader(ShaderID::ParticleUpdate_CS);
    if (!emitCS || !updateCS)
    {
        LOG_ERROR("ParticleSimPass: compute shader lookup failed");
        return;
    }

    {
        RHI::PipelineStateDesc d{};
        d.cs = emitCS;
        if (!gfx.CreatePipelineState(d, m_emitPSO))
            LOG_ERROR("ParticleSimPass: emit PSO failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = updateCS;
        if (!gfx.CreatePipelineState(d, m_updatePSO))
            LOG_ERROR("ParticleSimPass: update PSO failed");
    }

    LOG_SUCCESS("ParticleSimPass: initialised");
}

void ParticleSimPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_emitPSO.IsValid() || !m_updatePSO.IsValid()) return;
    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // ---- Phase A: Emit (one dispatch per emitter) --------------------------
    const uint32_t emitterCount = m_sys->GetEmitterCount();
    if (emitterCount > 0)
    {
        gfx.BindComputePipelineState(m_emitPSO, cl);
        gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, m_sys->GetPoolUAVHandle(), cl);

        // Mesh-shape sampling bindings (no-op for other shapes — root slots
        // stay unbound but shader won't read them unless shapeType=MESH).
        if (m_meshDescBuffer && m_meshDescBuffer->IsValid())
        {
            const uint64_t meshDescSrv = m_gfx->GetBufferSRVGpuHandle(*m_meshDescBuffer);
            gfx.SetComputeDescriptorTable(kCSMeshDescSlot, meshDescSrv, cl);
        }
        if (m_bindlessTableHandle)
            gfx.SetComputeDescriptorTable(kCSBindlessSlot, m_bindlessTableHandle, cl);

        for (uint32_t i = 0; i < emitterCount; ++i)
        {
            const uint32_t spawnCount = m_sys->GetEmitterSpawnCount(i);
            if (spawnCount == 0) continue;

            // Per-emitter 256B-aligned CBV offset.
            const uint32_t cbOffset = static_cast<uint32_t>(
                i * ParticleSystem::kEmitterSlotStride);
            gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetEmitterBuffer(), cbOffset, cl);

            gfx.DispatchCompute((spawnCount + 63) / 64, 1, 1, cl);
        }

        // UAV barrier: emit writes must land before update reads the same slots.
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetParticlePool()), cl);
    }

    // ---- Phase B: Update (one dispatch over the whole pool) ----------------
    gfx.BindComputePipelineState(m_updatePSO, cl);
    gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetSystemCB(), 0, cl);
    gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, m_sys->GetPoolUAVHandle(), cl);

    const uint32_t poolSize = ParticleSystem::GetPoolCapacity();
    gfx.DispatchCompute((poolSize + 63) / 64, 1, 1, cl);

    // Final UAV barrier so the subsequent render pass sees a consistent pool.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetParticlePool()), cl);
}

// ===========================================================================
// ParticleRenderPass
// ===========================================================================

ParticleRenderPass::ParticleRenderPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

void ParticleRenderPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth-test against GBuffer depth, no write.
    b.WriteDepthStencil(m_depth);
    // We bind the HDR RTV manually in Execute (same pattern as TransparentPass).
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void ParticleRenderPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Particle_VS, RHI::ShaderStage::VS, "Particle.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::Particle_PS, RHI::ShaderStage::PS, "Particle.ps.hlsl", "main");

    const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::Particle_VS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Particle_PS);
    if (!vs || !ps)
    {
        LOG_ERROR("ParticleRenderPass: shader lookup failed");
        return;
    }

    // Assemble the graphics PSO directly. Additive blend, depth-test on,
    // depth-write off, triangle-strip 4-vert billboards.
    RHI::RasterizerState   rs{};
    rs.fill_mode         = RHI::FillMode::SOLID;
    rs.cull_mode         = RHI::CullMode::NONE;
    rs.depth_clip_enable = true;

    RHI::DepthStencilState dss{};
    dss.depth_enable     = true;
    dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;   // reversed-Z
    dss.stencil_enable   = false;

    RHI::BlendState bs{};
    auto& rt = bs.render_target[0];
    rt.blend_enable   = true;
    // Additive: dstRGB = srcRGB * 1 + dstRGB * 1 (pre-multiplied by src alpha
    // via SRC_ALPHA source factor so fading particles dim naturally).
    rt.src_blend       = RHI::Blend::SRC_ALPHA;
    rt.dest_blend      = RHI::Blend::ONE;
    rt.blend_op        = RHI::BlendOp::ADD;
    rt.src_blend_alpha = RHI::Blend::ONE;
    rt.dest_blend_alpha= RHI::Blend::ONE;
    rt.blend_op_alpha  = RHI::BlendOp::ADD;
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    RHI::PipelineStateDesc d{};
    d.vs            = vs;
    d.ps            = ps;
    d.rs            = &rs;
    d.dss           = &dss;
    d.bs            = &bs;
    d.pt            = RHI::PrimitiveTopology::TRIANGLESTRIP;
    d.rtv_formats[0]= RHI::Format::R16G16B16A16_FLOAT;
    d.rtv_count     = 1;
    d.dsv_format    = RHI::Format::D32_FLOAT_S8X24_UINT;
    d.sample_count  = 1;

    if (!gfx.CreatePipelineState(d, m_pso))
    {
        LOG_ERROR("ParticleRenderPass: PSO creation failed");
        return;
    }

    // Per-frame render CB (root CBV at b2 space0, 256B aligned).
    if (!m_renderCB.Create(gfx, "ParticleRenderPass.RenderCB"))
        LOG_ERROR("ParticleRenderPass: render CB create failed");

    // s0: linear-wrap sampler for optional texture.
    RHI::SamplerDesc sd;  // defaults: MIN_MAG_MIP_LINEAR, WRAP
    if (!gfx.CreateSampler(sd, m_linearSamplerIdx))
        LOG_ERROR("ParticleRenderPass: sampler creation failed");

    LOG_SUCCESS("ParticleRenderPass: initialised");
}

RHI::CommandList ParticleRenderPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_pso.IsValid() || !m_renderCB.IsValid()) return cl;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Pack per-frame camera data into the CB. Matrix is transposed for the
    // HLSL row-vector convention (VS does `mul(pos, viewProj)` with row
    // semantics, so the GPU layout wants column-major == CPU transposed).
    using namespace DirectX;
    if (auto* slot = m_renderCB.Current(gfx))
    {
        RenderCB cb{};
        XMMATRIX vp = XMLoadFloat4x4(&m_viewProj);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),
                        XMMatrixTranspose(vp));
        cb.camRight[0]  = m_camRight.x;
        cb.camRight[1]  = m_camRight.y;
        cb.camRight[2]  = m_camRight.z;
        cb.camUp[0]     = m_camUp.x;
        cb.camUp[1]     = m_camUp.y;
        cb.camUp[2]     = m_camUp.z;
        cb.particleSize = m_particleSize;
        cb._pad         = 0.0f;
        *slot = cb;
    }

    // Bind the HDR RTV with GBuffer depth (read-only depth test).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    gfx.SetRenderTargetToHdrWithDepth(depthTex, cl);

    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPipelineState(m_pso);
    // PrimitiveTopology is NOT part of PSO state in D3D12 — it is a dynamic
    // per-draw state. Must be set explicitly; otherwise we inherit whatever
    // the previous pass set (TransparentPass uses TRIANGLELIST), and 4
    // vertices consumed as a list would draw only the first triangle,
    // producing a visible half-quad == a half circle of the alpha mask.
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLESTRIP);

    // Root CBV (b2 space0) = render CB.
    gfx.BindConstantBuffer(m_renderCB.CurrentBuffer(gfx), /*slot=*/1, cl);
    // Descriptor table (t4 space0) = particle pool SRV.
    cl.BindDescriptorTableHandle(kGfxPoolSRVSlot, m_sys->GetPoolSRVHandle());

    // Bindless texture table (t0 space2) — particles sample
    // g_AllTextures[p.textureBindlessIdx] when set.
    D3D12_GPU_DESCRIPTOR_HANDLE texTable = gfx.GetBindlessTextureTableHandle();
    if (texTable.ptr)
        cl.BindDescriptorTableHandle(kGfxBindlessTexSlot, texTable.ptr);

    // Linear sampler for texture sampling.
    if (m_linearSamplerIdx >= 0)
        cl.BindSampler(0, m_linearSamplerIdx);

    // Triangle strip: 4 verts per instance; one instance per pool slot.
    gfx.DrawInstanced(4, ParticleSystem::GetPoolCapacity(), 0, 0, cl);

    return cl;
}
