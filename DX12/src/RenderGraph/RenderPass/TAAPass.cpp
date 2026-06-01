#include "RenderGraph/RenderPass/TAAPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>

// Compute root signature slots (must match GraphicsDX12::CreateComputeRootSignature):
//   [0] ROOT_CBV   b0 space2
//   [1] DESC_TABLE  t0 space2  ← current HDR
//   [2] DESC_TABLE  t1 space2  ← depth
//   [3] DESC_TABLE  t2 space2  ← history
//   [4] DESC_TABLE  u0 space2  ← output UAV
//   [7] DESC_TABLE  t3 space2  ← GBuffer surface (roughness/metallic)
//   [8] DESC_TABLE  t4 space2  ← velocity
//   [12] DESC_TABLE t6 space2  ← prev-frame depth (ping-pong, reused root slot)
//   [13] DESC_TABLE t7 space2  ← prev-frame velocity (ping-pong, reused root slot)
//   [14] DESC_TABLE u2 space2  ← prev-depth out UAV (writes curr → next-frame prev)
//   [15] DESC_TABLE u3 space2  ← prev-velocity out UAV
static constexpr uint32_t kCBSlot          = 0;
static constexpr uint32_t kHdrSRV          = 1;
static constexpr uint32_t kDepthSRV        = 2;
static constexpr uint32_t kHistorySRV      = 3;
static constexpr uint32_t kOutputUAV       = 4;
// Slot 6 in the shared compute root signature is t5 space2 (originally
// used for SkinningPass morph weights). It is a generic SRV descriptor table
// so TAA can bind a texture SRV there for outline stencil reads — different
// PSOs bind different descriptor types to the same root slot without conflict.
static constexpr uint32_t kOutlineStencil  = 6;
static constexpr uint32_t kGBufferSRV      = 7;
static constexpr uint32_t kVelocitySRV     = 8;
static constexpr uint32_t kPrevDepthSRV    = 12;
static constexpr uint32_t kPrevVelocitySRV = 13;
static constexpr uint32_t kPrevDepthUAV    = 14;
static constexpr uint32_t kPrevVelocityUAV = 15;

// Must match OutlinePass::kOutlineStencilBit and the value the shader tests.
static constexpr uint32_t kOutlineStencilBitMask = 0x80u;

// ---------------------------------------------------------------------------
void TAAPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::TAA_CS, RHI::ShaderStage::CS, "TAA.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::TAA_CS);
    if (!cs)
    {
        LOG_ERROR("TAAPass: TAA_CS shader not found");
        return;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("TAAPass: PSO creation failed");
        return;
    }

    // Triple-buffered constant buffer — see Graphics/FrameCB.h. Required for
    // pipelined frame pacing so frame N+1's CPU write doesn't race frame N's
    // GPU read of the same UPLOAD memory.
    m_cb.Create(gfx, "TAA.CB");

    // 1x1 zero-velocity fallback (R16G16_FLOAT) — bound to the velocity slot
    // (t4 space2, root 8) when no GBuffer velocity SRV is available. TAA.cs reads
    // gVelocity unconditionally, so leaving slot 8 unbound trips GPU-Based
    // Validation "uninitialized root argument" every frame. (0,0) → zero
    // reprojection (matches the prior no-velocity behaviour).
    {
        const uint16_t zeroVel[2] = { 0, 0 };
        RHI::TextureDesc td{};
        td.width      = 1;
        td.height     = 1;
        td.format     = RHI::Format::R16G16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        RHI::SubresourceData sub{};
        sub.data_ptr    = zeroVel;
        sub.row_pitch   = sizeof(zeroVel);
        sub.slice_pitch = sizeof(zeroVel);
        if (gfx.CreateTexture(td, m_zeroVelocityTex, &sub))
            m_zeroVelocitySrv = gfx.GetTextureSRVGpuHandle(m_zeroVelocityTex);
        else
            LOG_ERROR("TAAPass: failed to create zero-velocity fallback");
    }

    LOG_SUCCESS("TAAPass: initialized");
}

// ---------------------------------------------------------------------------
void TAAPass::SetFrameData(const DirectX::XMFLOAT4X4& invVP,
                           const DirectX::XMFLOAT4X4& prevVP,
                           float historyWeight,
                           bool  hasHistory,
                           float deltaTime)
{
    std::memcpy(m_cbData.invViewProj,  &invVP,  sizeof(float) * 16);
    std::memcpy(m_cbData.prevViewProj, &prevVP, sizeof(float) * 16);
    m_cbData.historyWeight = historyWeight;
    m_cbData.hasHistory    = hasHistory ? 1.0f : 0.0f;
    m_cbData.deltaTime     = deltaTime;
}

// ---------------------------------------------------------------------------
uint64_t TAAPass::GetResolvedSrvHandle() const
{
    if (!m_gfx || !m_pingPong[m_writeIdx].IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_pingPong[m_writeIdx]);
}

// ---------------------------------------------------------------------------
void TAAPass::RebuildBuffers()
{
    if (!m_gfx || m_vpW == 0 || m_vpH == 0) return;

    for (int i = 0; i < 2; ++i)
    {
        if (m_pingPong[i].IsValid())
            m_gfx->DestroyTexture(m_pingPong[i]);

        RHI::TextureDesc td{};
        td.width      = m_vpW;
        td.height     = m_vpH;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

        if (!m_gfx->CreateTexture(td, m_pingPong[i]))
            LOG_ERROR("TAAPass: failed to create ping-pong buffer %d", i);
    }

    // Prev-depth ping-pong (R32_FLOAT, SRV+UAV).
    for (int i = 0; i < 2; ++i)
    {
        if (m_prevDepth[i].IsValid())
            m_gfx->DestroyTexture(m_prevDepth[i]);
        RHI::TextureDesc td{};
        td.width      = m_vpW;
        td.height     = m_vpH;
        td.format     = RHI::Format::R32_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!m_gfx->CreateTexture(td, m_prevDepth[i]))
            LOG_ERROR("TAAPass: failed to create prev-depth buffer %d", i);
    }

    // Prev-velocity ping-pong (R16G16_FLOAT, SRV+UAV).
    for (int i = 0; i < 2; ++i)
    {
        if (m_prevVelocity[i].IsValid())
            m_gfx->DestroyTexture(m_prevVelocity[i]);
        RHI::TextureDesc td{};
        td.width      = m_vpW;
        td.height     = m_vpH;
        td.format     = RHI::Format::R16G16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!m_gfx->CreateTexture(td, m_prevVelocity[i]))
            LOG_ERROR("TAAPass: failed to create prev-velocity buffer %d", i);
    }

    // After rebuild: all start as UAV (creation state).
    m_resolvedState = RHI::ResourceState::UNORDERED_ACCESS;
    m_historyState  = RHI::ResourceState::UNORDERED_ACCESS;
    m_prevDepthWriteState    = RHI::ResourceState::UNORDERED_ACCESS;
    m_prevDepthReadState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_prevVelocityWriteState = RHI::ResourceState::UNORDERED_ACCESS;
    m_prevVelocityReadState  = RHI::ResourceState::UNORDERED_ACCESS;
    m_hasHistory    = false;
    m_writeIdx      = 0;
    m_lastVpW       = m_vpW;
    m_lastVpH       = m_vpH;

    LOG_INFO("TAAPass: rebuilt ping-pong buffers %ux%u", m_vpW, m_vpH);
}

// ---------------------------------------------------------------------------
RHI::CommandList TAAPass::Execute(RHI::CommandList cl)
{
    // Disabled via editor toggle — skip resolve. Downstream passes are
    // routed to the raw HDR scene SRV in Renderer::Render so they still
    // see a correct color input.
    if (!m_enabled) return cl;

    if (!m_pso.IsValid() || m_vpW == 0 || m_vpH == 0) return cl;

    // Resize on viewport change.
    if (m_vpW != m_lastVpW || m_vpH != m_lastVpH)
        RebuildBuffers();

    if (!m_pingPong[0].IsValid() || !m_pingPong[1].IsValid()) return cl;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // ---- Select ping-pong roles this frame ---------------------------------
    int readIdx  = 1 - m_writeIdx;   // history = other buffer
    int writeIdx = m_writeIdx;       // resolved = this buffer

    // ---- Upload TAACB -------------------------------------------------------
    m_cbData.width  = m_vpW;
    m_cbData.height = m_vpH;
    if (!m_hasHistory) m_cbData.hasHistory = 0.0f;  // override if no history
    m_cbData.colorBoxSigma         = colorBoxSigma;
    m_cbData.colorBoxSigmaSpecular = colorBoxSigmaSpecular;
    m_cbData.specularRoughnessMax  = specularRoughnessMax;
    m_cbData.antiFlicker           = antiFlicker ? 1u : 0u;
    m_cbData.velocityWiden         = velocityWiden;
    m_cbData.sharpenStrength       = sharpenStrength;
    m_cbData.outlineMinAlpha       = outlineMinAlpha;
    // Disable the stencil lookup when no SRV was plumbed in — the shader keys
    // off this bit being non-zero before sampling, so an unbound stencil
    // descriptor is harmless.
    m_cbData.outlineStencilBit     = m_outlineStencilSrvHandle ? kOutlineStencilBitMask : 0u;
    if (auto* slot = m_cb.Current(gfx))
        *slot = m_cbData;

    // ---- Transition write buffer to UAV for dispatch output -----------------
    if (m_resolvedState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_pingPong[writeIdx],
            m_resolvedState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_resolvedState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // ---- Transition history buffer to SRV for sampling ---------------------
    // Always transition, even on the first frame (m_hasHistory == false).
    // The shader ignores the history sample when hasHistory == 0, but the
    // resource must still be in a valid SRV state before the descriptor is bound.
    if (m_historyState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_pingPong[readIdx],
            m_historyState,
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_historyState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }

    // ---- Transition prev depth/velocity: read=SRV, write=UAV ---------------
    if (m_prevDepthReadState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_prevDepth[readIdx],
            m_prevDepthReadState,
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_prevDepthReadState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }
    if (m_prevDepthWriteState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_prevDepth[writeIdx],
            m_prevDepthWriteState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_prevDepthWriteState = RHI::ResourceState::UNORDERED_ACCESS;
    }
    if (m_prevVelocityReadState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_prevVelocity[readIdx],
            m_prevVelocityReadState,
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_prevVelocityReadState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }
    if (m_prevVelocityWriteState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_prevVelocity[writeIdx],
            m_prevVelocityWriteState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_prevVelocityWriteState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // ---- Dispatch ----------------------------------------------------------
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);

    if (m_hdrSrvHandle)
        gfx.SetComputeDescriptorTable(kHdrSRV, m_hdrSrvHandle, cl);
    if (m_depthSrvHandle)
        gfx.SetComputeDescriptorTable(kDepthSRV, m_depthSrvHandle, cl);
    if (m_gbufferSrvHandle)
        gfx.SetComputeDescriptorTable(kGBufferSRV, m_gbufferSrvHandle, cl);
    // Always bind slot 8 — TAA.cs reads gVelocity unconditionally; fall back to
    // a zero-velocity texture (no reprojection) when no GBuffer velocity SRV is
    // available, so the root argument is never left uninitialized (GBV).
    gfx.SetComputeDescriptorTable(kVelocitySRV,
        m_velocitySrvHandle ? m_velocitySrvHandle : m_zeroVelocitySrv, cl);
    if (m_outlineStencilSrvHandle)
        gfx.SetComputeDescriptorTable(kOutlineStencil, m_outlineStencilSrvHandle, cl);

    // Bind history (even on first frame — shader ignores it when hasHistory=0).
    const uint64_t historySrv = m_gfx->GetTextureSRVGpuHandle(m_pingPong[readIdx]);
    if (historySrv)
        gfx.SetComputeDescriptorTable(kHistorySRV, historySrv, cl);

    const uint64_t outputUav = gfx.GetTextureUAVGpuHandle(m_pingPong[writeIdx]);
    if (outputUav)
        gfx.SetComputeDescriptorTable(kOutputUAV, outputUav, cl);

    // Prev depth/velocity bindings (read=SRV at readIdx, write=UAV at writeIdx).
    const uint64_t prevDepthSrv = m_gfx->GetTextureSRVGpuHandle(m_prevDepth[readIdx]);
    if (prevDepthSrv)
        gfx.SetComputeDescriptorTable(kPrevDepthSRV, prevDepthSrv, cl);
    const uint64_t prevVelSrv   = m_gfx->GetTextureSRVGpuHandle(m_prevVelocity[readIdx]);
    if (prevVelSrv)
        gfx.SetComputeDescriptorTable(kPrevVelocitySRV, prevVelSrv, cl);
    const uint64_t prevDepthUav = gfx.GetTextureUAVGpuHandle(m_prevDepth[writeIdx]);
    if (prevDepthUav)
        gfx.SetComputeDescriptorTable(kPrevDepthUAV, prevDepthUav, cl);
    const uint64_t prevVelUav   = gfx.GetTextureUAVGpuHandle(m_prevVelocity[writeIdx]);
    if (prevVelUav)
        gfx.SetComputeDescriptorTable(kPrevVelocityUAV, prevVelUav, cl);

    gfx.DispatchCompute((m_vpW + 7) / 8, (m_vpH + 7) / 8, 1, cl);

    // ---- Transition resolved buffer to SRV for downstream passes -----------
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_pingPong[writeIdx],
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_resolvedState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // Track history buffer's state for next frame.
    // After this frame, the write buffer becomes next frame's history,
    // and its state is SHADER_RESOURCE_COMPUTE (set above).
    m_historyState  = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // Prev-depth/velocity: this frame's UAV write becomes next frame's SRV
    // read. Transition write→SRV; the readIdx slot stays SRV (no change).
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_prevDepth[writeIdx],
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_prevVelocity[writeIdx],
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_prevDepthWriteState    = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    m_prevDepthReadState     = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    m_prevVelocityWriteState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    m_prevVelocityReadState  = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // ---- Swap ping-pong for next frame -------------------------------------
    m_writeIdx  = readIdx;   // next write goes to the buffer we just read from
    m_hasHistory = true;

    return cl;
}
