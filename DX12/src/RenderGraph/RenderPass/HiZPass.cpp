#include "RenderGraph/RenderPass/HiZPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>
#include <algorithm>

// Reuse compute root sig slots (space2)
static constexpr uint32_t kCBSlot  = 0;  // b0 space2
static constexpr uint32_t kSRV0    = 1;  // t0 space2 — source mip (mip 0 path: depth SRV)
static constexpr uint32_t kUAV0    = 4;  // u0 space2 — dest (mip 0) OR source (reduce)
static constexpr uint32_t kUAV1    = 5;  // u1 space2 — dest mip (reduce path)

void HiZPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::HiZGenerate_CS, RHI::ShaderStage::CS,
                         "HiZGenerate.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::HiZReduce_CS, RHI::ShaderStage::CS,
                         "HiZReduce.cs.hlsl", "CSMain");

    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::HiZGenerate_CS);
        if (!cs) { LOG_ERROR("HiZPass: HiZGenerate_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_pso))
        { LOG_ERROR("HiZPass: generate PSO creation failed"); return; }
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::HiZReduce_CS);
        if (!cs) { LOG_ERROR("HiZPass: HiZReduce_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_reducePSO))
        { LOG_ERROR("HiZPass: reduce PSO creation failed"); return; }
    }

    // CB
    RHI::GPUBufferDesc bd{};
    bd.size       = 256; // padded to 256-byte alignment
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    LOG_SUCCESS("HiZPass: initialized");
}

void HiZPass::EnsureTexture(uint32_t depthW, uint32_t depthH)
{
    if (!m_gfx) return;

    // Round up to next power of 2
    auto nextPow2 = [](uint32_t v) -> uint32_t {
        v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; return v + 1;
    };

    uint32_t w = nextPow2(depthW);
    uint32_t h = nextPow2(depthH);

    if (w == m_hiZWidth && h == m_hiZHeight && m_hiZTexture.IsValid()) return;

    if (m_hiZTexture.IsValid())
        m_gfx->DestroyTexture(m_hiZTexture);

    m_hiZWidth  = w;
    m_hiZHeight = h;

    // Compute mip count
    m_mipCount = 1;
    { uint32_t dim = std::max(w, h); while (dim > 1) { dim >>= 1; m_mipCount++; } }

    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.mip_levels = m_mipCount;
    td.format     = RHI::Format::R32_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, m_hiZTexture))
        LOG_ERROR("HiZPass: failed to create Hi-Z texture (%ux%u, %u mips)", w, h, m_mipCount);
    else
        LOG_INFO("HiZPass: created Hi-Z %ux%u (%u mips)", w, h, m_mipCount);
}

uint64_t HiZPass::GetHiZSrvHandle() const
{
    if (!m_gfx || !m_hiZTexture.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_hiZTexture);
}

void HiZPass::Execute(RHI::CommandList cl, uint64_t depthSrvHandle)
{
    if (!m_pso.IsValid() || !m_reducePSO.IsValid()) return;
    if (!m_hiZTexture.IsValid() || m_mipCount == 0) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // ---- Mip 0: depth SRV → Hi-Z mip 0 (HiZGenerate_CS) ---------------------
    // Depth and Hi-Z mip 0 share the same (source) dimensions: the shader
    // point-samples the depth texel at DTid coordinates and writes a 1:1 copy.
    // dstW/H in the CB match srcW/H for this pass; srcW/H gates the early-out.
    {
        const uint32_t w = m_hiZWidth;
        const uint32_t h = m_hiZHeight;

        gfx.BindComputePipelineState(m_pso, cl);

        if (m_cbMapped)
        {
            HiZCB cb{ w, h, w, h };
            std::memcpy(m_cbMapped, &cb, sizeof(cb));
        }
        gfx.SetComputeRootCBV(kCBSlot, m_cb, cl);
        gfx.SetComputeDescriptorTable(kSRV0, depthSrvHandle, cl);
        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_hiZTexture, 0), cl);

        gfx.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_hiZTexture), cl);
    }

    // ---- Mips 1..N-1: UAV→UAV reduce (HiZReduce_CS) ------------------------
    // Bind HiZReduce PSO once; reuse across all reduce passes by swapping only
    // the per-mip UAV tables + CB contents. Texture stays in UNORDERED_ACCESS
    // throughout — UAV barriers between dispatches serialize the read/write.
    gfx.BindComputePipelineState(m_reducePSO, cl);

    for (uint32_t mip = 1; mip < m_mipCount; ++mip)
    {
        const uint32_t dstW = (std::max)(m_hiZWidth  >> mip, 1u);
        const uint32_t dstH = (std::max)(m_hiZHeight >> mip, 1u);

        // Reduce shader reads srcWidth/srcHeight from CB (= this mip's size)
        // and writes to DTid coordinates bounded by that. CB layout is shared
        // with HiZGenerate_CS; dstW/dstH fields are ignored by Reduce.
        if (m_cbMapped)
        {
            HiZCB cb{ dstW, dstH, dstW, dstH };
            std::memcpy(m_cbMapped, &cb, sizeof(cb));
        }
        gfx.SetComputeRootCBV(kCBSlot, m_cb, cl);

        // u0 = source (prev mip, read-only UAV), u1 = dest (this mip).
        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_hiZTexture, mip - 1), cl);
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureMipUAVGpuHandle(m_hiZTexture, mip), cl);

        gfx.DispatchCompute((dstW + 7) / 8, (dstH + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_hiZTexture), cl);
    }
}
