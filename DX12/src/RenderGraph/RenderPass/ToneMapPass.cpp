#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>

// Compute root signature slot assignments (shared across all compute passes)
static constexpr uint32_t kCBSlot  = 0;  // CBV  b0 space2
static constexpr uint32_t kSRV0    = 1;  // SRV  t0 space2 — HDR scene
static constexpr uint32_t kSRV1    = 2;  // SRV  t1 space2 — bloom
static constexpr uint32_t kSRV2    = 3;  // SRV  t2 space2 — exposure buffer
static constexpr uint32_t kUAV0    = 4;  // UAV  u0 space2 — final output
static constexpr uint32_t kUAV1    = 5;  // UAV  u1 space2 — LUT 3D (bake target)
static constexpr uint32_t kSRV5    = 6;  // SRV  t5 space2 — LUT 3D (tonemap read)
static constexpr uint32_t kSRV3    = 7;  // SRV  t3 space2 — lens flare additive

static constexpr uint32_t kLutSize = 32;

// ---------------------------------------------------------------------------
void ToneMapPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");

    // Tonemap shader
    m_shaderLib.Register(ShaderID::ToneMap_CS, RHI::ShaderStage::CS,
                         "ToneMap.cs.hlsl", "CSMain");
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::ToneMap_CS);
    if (!cs) { LOG_ERROR("ToneMapPass: ToneMap_CS not found"); return; }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("ToneMapPass: tonemap PSO failed"); return; }

    // GenerateLUT shader
    m_shaderLib.Register(ShaderID::GenerateLUT_CS, RHI::ShaderStage::CS,
                         "GenerateLUT.cs.hlsl", "CSMain");
    const RHI::Shader* lutCs = m_shaderLib.GetShader(ShaderID::GenerateLUT_CS);
    if (!lutCs) { LOG_ERROR("ToneMapPass: GenerateLUT_CS not found"); return; }

    RHI::PipelineStateDesc ld{};
    ld.cs = lutCs;
    if (!gfx.CreatePipelineState(ld, m_lutPso))
    { LOG_ERROR("ToneMapPass: LUT PSO failed"); return; }

    // Tonemap CB
    m_cb.Create(gfx, "ToneMap.CB");

    // Color grading CB
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = (sizeof(ColorGradingParams) + 255) & ~255u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_gradingCb))
            m_gradingCbMapped = gfx.MapBuffer(m_gradingCb);
    }

    // Create the 3D LUT texture
    CreateLUT();

    LOG_SUCCESS("ToneMapPass: initialized (with procedural LUT)");
}

// ---------------------------------------------------------------------------
void ToneMapPass::CreateLUT()
{
    if (!m_gfx) return;

    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
    td.width      = kLutSize;
    td.height     = kLutSize;
    td.depth      = kLutSize;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, m_lutTexture))
        LOG_ERROR("ToneMapPass: failed to create LUT 3D texture");

    m_lutState = RHI::ResourceState::UNORDERED_ACCESS;
    m_lutDirty = true;
    LOG_INFO("ToneMapPass: created %ux%ux%u LUT (R16G16B16A16_FLOAT)", kLutSize, kLutSize, kLutSize);
}

// ---------------------------------------------------------------------------
void ToneMapPass::BakeLUT(RHI::CommandList cl)
{
    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Upload grading params
    if (m_gradingCbMapped)
        std::memcpy(m_gradingCbMapped, &m_gradingParams, sizeof(ColorGradingParams));

    // Ensure LUT is in UAV state
    if (m_lutState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_lutTexture, m_lutState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_lutState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // Dispatch LUT bake
    gfx.BindComputePipelineState(m_lutPso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_gradingCb, cl);
    gfx.SetComputeDescriptorTable(kUAV1, gfx.GetTextureUAVGpuHandle(m_lutTexture), cl);
    gfx.DispatchCompute(kLutSize / 4, kLutSize / 4, kLutSize / 4, cl);

    // UAV barrier → transition to SRV for tonemap read
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_lutTexture, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_lutState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    m_lutDirty = false;
}

// ---------------------------------------------------------------------------
void ToneMapPass::RebuildTexture()
{
    if (!m_gfx) return;
    if (m_finalOutput.IsValid()) m_gfx->DestroyTexture(m_finalOutput);

    RHI::TextureDesc td{};
    td.width      = m_vpW;
    td.height     = m_vpH;
    td.format     = RHI::Format::R8G8B8A8_UNORM;
    // RENDER_TARGET added so UIPass can transition to RT and bind via
    // OMSetRenderTargets to draw HUD/menus on top of the tone-mapped scene.
    // SR+UAV+RT flags coexist on the same texture without performance impact;
    // the underlying D3D12 resource just allocates one extra RTV descriptor.
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE
                  | RHI::BindFlag::UNORDERED_ACCESS
                  | RHI::BindFlag::RENDER_TARGET;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debug_name = "ToneMapPass.FinalOutput";
    if (!m_gfx->CreateTexture(td, m_finalOutput))
        LOG_ERROR("ToneMapPass: failed to create final output texture");

    // GraphicsDX12::CreateTexture forces InitialState=RENDER_TARGET whenever
    // bind_flags includes RT (regardless of `td.layout`).  Track that real
    // state — first ToneMap dispatch emits the RT→UAV transition barrier
    // implicitly via the `if (state != UAV)` check at the top of Execute.
    m_finalOutputState = RHI::ResourceState::RENDERTARGET;
    m_lastVpW = m_vpW;
    m_lastVpH = m_vpH;
    LOG_INFO("ToneMapPass: rebuilt final output %ux%u", m_vpW, m_vpH);
}

// ---------------------------------------------------------------------------
uint64_t ToneMapPass::GetFinalOutputSrvHandle() const
{
    if (!m_gfx || !m_finalOutput.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_finalOutput);
}

// ---------------------------------------------------------------------------
RHI::CommandList ToneMapPass::Execute(RHI::CommandList cl)
{
    if (!m_pso.IsValid()) return cl;
    if (m_vpW == 0 || m_vpH == 0) return cl;

    if (m_vpW != m_lastVpW || m_vpH != m_lastVpH)
        RebuildTexture();
    if (!m_finalOutput.IsValid()) return cl;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // ---- Step 1: Bake LUT if dirty ----
    if (m_lutDirty && m_lutTexture.IsValid() && m_colorGradingEnabled)
        BakeLUT(cl);

    // ---- Step 2: Tonemap dispatch ----
    if (auto* slot = m_cb.Current(gfx))
    {
        ToneMapPass::ToneMapCB cb{};
        cb.width             = m_vpW;
        cb.height            = m_vpH;
        cb.bloomStrength     = m_bloomStrength;
        cb.enableLUT         = (m_colorGradingEnabled && m_lutTexture.IsValid()) ? 1u : 0u;
        cb.lensFlareStrength = (m_lensFlareSrvHandle != 0) ? m_lensFlareStrength : 0.0f;
        *slot = cb;
    }

    // Transition final output to UAV
    if (m_finalOutputState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_finalOutput, m_finalOutputState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_finalOutputState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRV0, m_hdrSrvHandle, cl);
    gfx.SetComputeDescriptorTable(kSRV1, m_bloomSrvHandle, cl);
    gfx.SetComputeDescriptorTable(kSRV2, m_exposureSrvHandle, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_finalOutput), cl);

    // Bind LUT SRV for tonemap read
    if (m_colorGradingEnabled && m_lutTexture.IsValid())
        gfx.SetComputeDescriptorTable(kSRV5, gfx.GetTextureSRVGpuHandle(m_lutTexture), cl);

    // Bind lens flare SRV (t3 space2). Flare strength CB controls whether the
    // shader actually adds the texture; binding 0 is safe as the strength CB
    // will be 0.0f in that case.
    if (m_lensFlareSrvHandle != 0)
        gfx.SetComputeDescriptorTable(kSRV3, m_lensFlareSrvHandle, cl);

    gfx.DispatchCompute((m_vpW + 7) / 8, (m_vpH + 7) / 8, 1, cl);

    // Transition final output for next stage
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_finalOutput, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_finalOutputState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    return cl;
}

// ---------------------------------------------------------------------------
void ToneMapPass::TransitionForDisplay(RHI::CommandList& cl)
{
    if (!m_finalOutput.IsValid()) return;
    if (m_finalOutputState == RHI::ResourceState::SHADER_RESOURCE) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_finalOutput, m_finalOutputState,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_finalOutputState = RHI::ResourceState::SHADER_RESOURCE;
}

// ---------------------------------------------------------------------------
void ToneMapPass::PrepareForCompute(RHI::CommandList& cl)
{
    if (!m_finalOutput.IsValid()) return;
    if (m_finalOutputState == RHI::ResourceState::UNORDERED_ACCESS) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_finalOutput, m_finalOutputState,
        RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_finalOutputState = RHI::ResourceState::UNORDERED_ACCESS;
}
