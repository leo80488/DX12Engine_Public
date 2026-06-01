#include "RenderGraph/RenderPass/BloomPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>

// Compute root sig slots (match CreateComputeRootSignature)
static constexpr uint32_t kCBSlot = 0;
static constexpr uint32_t kSRV0   = 1;
static constexpr uint32_t kUAV0   = 4;

struct BloomCB
{
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
    float    filterRadius;
    uint32_t firstDownsample;
    float    pad0;
    float    pad1;
};

// ---------------------------------------------------------------------------
void BloomPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::BloomDownsample_CS, RHI::ShaderStage::CS,
                         "Bloom.cs.hlsl", "CSDownsample");
    m_shaderLib.Register(ShaderID::BloomUpsample_CS, RHI::ShaderStage::CS,
                         "Bloom.cs.hlsl", "CSUpsample");

    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::BloomDownsample_CS);
        if (!cs) { LOG_ERROR("BloomPass: BloomDownsample_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_downsamplePSO))
            LOG_ERROR("BloomPass: downsample PSO failed");
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::BloomUpsample_CS);
        if (!cs) { LOG_ERROR("BloomPass: BloomUpsample_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_upsamplePSO))
            LOG_ERROR("BloomPass: upsample PSO failed");
    }

    m_cb.Create(gfx, "Bloom.CB");

    LOG_SUCCESS("BloomPass: initialized");
}

// ---------------------------------------------------------------------------
void BloomPass::DestroyTextures()
{
    if (!m_gfx) return;
    for (int i = 0; i < kSteps; ++i)
        if (m_bloom[i].IsValid()) m_gfx->DestroyTexture(m_bloom[i]);
}

void BloomPass::RebuildTextures()
{
    if (!m_gfx) return;
    DestroyTextures();

    uint32_t w = (m_vpW + 1) / 2;
    uint32_t h = (m_vpH + 1) / 2;
    for (int i = 0; i < kSteps; ++i)
    {
        m_bloomW[i] = (w < 1) ? 1 : w;
        m_bloomH[i] = (h < 1) ? 1 : h;

        RHI::TextureDesc td{};
        td.width      = m_bloomW[i];
        td.height     = m_bloomH[i];
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!m_gfx->CreateTexture(td, m_bloom[i]))
            LOG_ERROR("BloomPass: failed to create bloom[%d]", i);

        m_bloomState[i] = RHI::ResourceState::UNORDERED_ACCESS;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    m_lastVpW = m_vpW;
    m_lastVpH = m_vpH;
    LOG_INFO("BloomPass: rebuilt bloom chain %ux%u", m_vpW, m_vpH);
}

// ---------------------------------------------------------------------------
uint64_t BloomPass::GetBloomSrvHandle() const
{
    if (!m_gfx || !m_bloom[0].IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_bloom[0]);
}

// ---------------------------------------------------------------------------
RHI::CommandList BloomPass::Execute(RHI::CommandList cl)
{
    if (!m_downsamplePSO.IsValid() || !m_upsamplePSO.IsValid()) return cl;
    if (m_vpW == 0 || m_vpH == 0) return cl;
    if (m_hdrSrvHandle == 0) return cl;

    if (m_vpW != m_lastVpW || m_vpH != m_lastVpH)
        RebuildTextures();
    if (!m_bloom[0].IsValid()) return cl;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    auto* cbMapped = m_cb.Current(gfx);
    uint8_t* cbBase = cbMapped ? cbMapped->bytes : nullptr;
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(gfx);

    // Helper to transition a bloom texture
    auto transitionBloom = [&](int i, RHI::ResourceState to)
    {
        if (m_bloomState[i] == to) return;
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_bloom[i], m_bloomState[i], to), cl);
        m_bloomState[i] = to;
    };

    // Restore any bloom textures left in SRV state from last frame
    for (int i = 0; i < kSteps; ++i)
        transitionBloom(i, RHI::ResourceState::UNORDERED_ACCESS);

    // --- Downsample phase ---
    gfx.BindComputePipelineState(m_downsamplePSO, cl);

    uint32_t cbOffsetIndex = 0;

    for (int i = 0; i < kSteps; ++i)
    {
        uint32_t srcW = (i == 0) ? m_vpW    : m_bloomW[i - 1];
        uint32_t srcH = (i == 0) ? m_vpH    : m_bloomH[i - 1];

        // Transition input bloom[i-1] to SRV if needed
        if (i > 0)
            transitionBloom(i - 1, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

        BloomCB cb{};
        cb.srcWidth       = srcW;
        cb.srcHeight      = srcH;
        cb.dstWidth       = m_bloomW[i];
        cb.dstHeight      = m_bloomH[i];
        cb.filterRadius   = 1.0f;
        cb.firstDownsample = (i == 0) ? 1u : 0u;
        
        uint32_t byteOffset = cbOffsetIndex * 256;
        if (cbBase) std::memcpy(cbBase + byteOffset, &cb, sizeof(cb));

        gfx.SetComputeRootCBV(kCBSlot, cbBuf, byteOffset, cl);
        cbOffsetIndex++;

        uint64_t srvHandle = (i == 0) ? m_hdrSrvHandle
                                      : gfx.GetTextureSRVGpuHandle(m_bloom[i - 1]);
        gfx.SetComputeDescriptorTable(kSRV0, srvHandle, cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_bloom[i]), cl);

        gfx.DispatchCompute((m_bloomW[i] + 7) / 8, (m_bloomH[i] + 7) / 8, 1, cl);

        // UAV barrier for the output
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_bloom[i]), cl);
    }

    // --- Upsample phase (bloom[4] → bloom[3] → ... → bloom[0]) ---
    gfx.BindComputePipelineState(m_upsamplePSO, cl);

    for (int i = kSteps - 1; i > 0; --i)
    {
        // Transition bloom[i] to SRV for reading
        transitionBloom(i, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        // Transition bloom[i-1] to UAV for writing (it may be in SRV from downsample)
        transitionBloom(i - 1, RHI::ResourceState::UNORDERED_ACCESS);

        BloomCB cb{};
        cb.srcWidth      = m_bloomW[i];
        cb.srcHeight     = m_bloomH[i];
        cb.dstWidth      = m_bloomW[i - 1];
        cb.dstHeight     = m_bloomH[i - 1];
        cb.filterRadius  = 1.0f; // Used in upsample 
        cb.firstDownsample = 0;
        
        uint32_t byteOffset = cbOffsetIndex * 256;
        if (cbBase) std::memcpy(cbBase + byteOffset, &cb, sizeof(cb));

        gfx.SetComputeRootCBV(kCBSlot, cbBuf, byteOffset, cl);
        cbOffsetIndex++;
        gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_bloom[i]), cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_bloom[i - 1]), cl);

        gfx.DispatchCompute((m_bloomW[i-1] + 7) / 8, (m_bloomH[i-1] + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_bloom[i - 1]), cl);
    }

    // Transition bloom[0] to SRV so ToneMapPass can read it
    transitionBloom(0, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

    return cl;
}
