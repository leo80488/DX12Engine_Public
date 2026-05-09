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
static constexpr uint32_t kCBSlot       = 0;
static constexpr uint32_t kHdrSRV       = 1;
static constexpr uint32_t kDepthSRV     = 2;
static constexpr uint32_t kHistorySRV   = 3;
static constexpr uint32_t kOutputUAV    = 4;
static constexpr uint32_t kGBufferSRV   = 7;
static constexpr uint32_t kVelocitySRV  = 8;  // t4 space2 — velocity buffer

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

    // Persistently mapped constant buffer.
    RHI::GPUBufferDesc bd{};
    bd.size       = (sizeof(TAACB) + 255u) & ~255u;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    LOG_SUCCESS("TAAPass: initialized");
}

// ---------------------------------------------------------------------------
void TAAPass::SetFrameData(const DirectX::XMFLOAT4X4& invVP,
                           const DirectX::XMFLOAT4X4& prevVP,
                           float tauHistory,
                           bool  hasHistory,
                           float deltaTime)
{
    std::memcpy(m_cbData.invViewProj,  &invVP,  sizeof(float) * 16);
    std::memcpy(m_cbData.prevViewProj, &prevVP, sizeof(float) * 16);
    m_cbData.tauHistory = tauHistory;
    m_cbData.hasHistory = hasHistory ? 1.0f : 0.0f;
    m_cbData.deltaTime  = deltaTime;
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

    // After rebuild: both start as UAV (creation state).
    m_resolvedState = RHI::ResourceState::UNORDERED_ACCESS;
    m_historyState  = RHI::ResourceState::UNORDERED_ACCESS;
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
    if (m_cbMapped)
        std::memcpy(m_cbMapped, &m_cbData, sizeof(TAACB));

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

    // ---- Dispatch ----------------------------------------------------------
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb, cl);

    if (m_hdrSrvHandle)
        gfx.SetComputeDescriptorTable(kHdrSRV, m_hdrSrvHandle, cl);
    if (m_depthSrvHandle)
        gfx.SetComputeDescriptorTable(kDepthSRV, m_depthSrvHandle, cl);
    if (m_gbufferSrvHandle)
        gfx.SetComputeDescriptorTable(kGBufferSRV, m_gbufferSrvHandle, cl);
    if (m_velocitySrvHandle)
        gfx.SetComputeDescriptorTable(kVelocitySRV, m_velocitySrvHandle, cl);

    // Bind history (even on first frame — shader ignores it when hasHistory=0).
    const uint64_t historySrv = m_gfx->GetTextureSRVGpuHandle(m_pingPong[readIdx]);
    if (historySrv)
        gfx.SetComputeDescriptorTable(kHistorySRV, historySrv, cl);

    const uint64_t outputUav = gfx.GetTextureUAVGpuHandle(m_pingPong[writeIdx]);
    if (outputUav)
        gfx.SetComputeDescriptorTable(kOutputUAV, outputUav, cl);

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

    // ---- Swap ping-pong for next frame -------------------------------------
    m_writeIdx  = readIdx;   // next write goes to the buffer we just read from
    m_hasHistory = true;

    return cl;
}
