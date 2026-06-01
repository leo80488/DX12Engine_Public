#include "Graphics/SSR/SSRDepthHierarchyPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

namespace
{
    // Shared compute root sig layout (space2). Same slot numbering as HiZPass.
    constexpr uint32_t kCBSlot    = 0;     // b0 space2
    constexpr uint32_t kSRV0      = 1;     // t0 space2 — depth (mip 0 path)
    constexpr uint32_t kUAV0      = 4;     // u0 space2 — dst mip0 / src mipN-1
    constexpr uint32_t kUAV1      = 5;     // u1 space2 — dst mipN
}

void SSRDepthHierarchyPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRDepthHierarchyMip0_CS, RHI::ShaderStage::CS,
                         "SSRDepthHierarchy.cs.hlsl", "CSMain_Mip0");
    m_shaderLib.Register(ShaderID::SSRDepthHierarchyReduce_CS, RHI::ShaderStage::CS,
                         "SSRDepthHierarchy.cs.hlsl", "CSMain_Reduce");

    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRDepthHierarchyMip0_CS);
        if (!cs) { LOG_ERROR("SSRDepthHierarchyPass: mip0 CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_mip0PSO))
        { LOG_ERROR("SSRDepthHierarchyPass: mip0 PSO creation failed"); return; }
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRDepthHierarchyReduce_CS);
        if (!cs) { LOG_ERROR("SSRDepthHierarchyPass: reduce CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_reducePSO))
        { LOG_ERROR("SSRDepthHierarchyPass: reduce PSO creation failed"); return; }
    }

    // One 256-byte CB slot per mip — see SceneColorPyramidPass for the race
    // story. Reusing a single slot across N dispatches recorded into the same
    // CL causes every reduce dispatch to read the LAST iteration's dims, so
    // every middle Hi-Z mip stays at its 0-init state.
    m_cb.Create(gfx, "SSRDepthHierarchy.CB");

    LOG_SUCCESS("SSRDepthHierarchyPass: initialized");
}

void SSRDepthHierarchyPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    auto rebuild = [&](ShaderID id, RHI::PipelineState& pso, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("SSRDepthHierarchyPass::ReloadShaders: %s missing", tag); return; }
        RHI::PipelineStateDesc d{}; d.cs = cs;
        if (!gfx.CreatePipelineState(d, pso))
            LOG_ERROR("SSRDepthHierarchyPass::ReloadShaders: %s PSO rebuild failed", tag);
    };
    rebuild(ShaderID::SSRDepthHierarchyMip0_CS,   m_mip0PSO,   "mip0");
    rebuild(ShaderID::SSRDepthHierarchyReduce_CS, m_reducePSO, "reduce");
}

void SSRDepthHierarchyPass::EnsureTexture(uint32_t depthW, uint32_t depthH)
{
    if (!m_gfx || depthW == 0 || depthH == 0) return;

    if (depthW == m_width && depthH == m_height && m_texture.IsValid()) return;

    if (m_texture.IsValid())
        m_gfx->DestroyTexture(m_texture);

    m_width  = depthW;
    m_height = depthH;

    m_mipCount = 1;
    { uint32_t dim = std::max(depthW, depthH); while (dim > 1) { dim >>= 1; m_mipCount++; } }

    RHI::TextureDesc td{};
    td.width      = depthW;
    td.height     = depthH;
    td.mip_levels = m_mipCount;
    td.format     = RHI::Format::R16G16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, m_texture))
        LOG_ERROR("SSRDepthHierarchyPass: texture creation failed (%ux%u, %u mips)",
                  depthW, depthH, m_mipCount);
    else
        LOG_INFO("SSRDepthHierarchyPass: created pyramid %ux%u (%u mips)",
                 depthW, depthH, m_mipCount);
}

uint64_t SSRDepthHierarchyPass::GetSrvHandle() const
{
    if (!m_gfx || !m_texture.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_texture);
}

void SSRDepthHierarchyPass::Execute(RHI::CommandList cl, uint64_t depthSrvHandle)
{
    if (!m_mip0PSO.IsValid() || !m_reducePSO.IsValid()) return;
    if (!m_texture.IsValid() || m_mipCount == 0 || !m_cb.IsValid()) return;
    if (!depthSrvHandle) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    auto* mapped = m_cb.Current(gfx);
    if (!mapped) return;
    uint8_t* cbBase = mapped->bytes;
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(gfx);

    // ---- Mip 0: depth SRV → pyramid mip 0 -----------------------------------
    {
        const uint32_t w = m_width;
        const uint32_t h = m_height;

        gfx.BindComputePipelineState(m_mip0PSO, cl);

        // Mip 0 uses CB slot 0.
        HierCB cb{ w, h, w, h };
        std::memcpy(cbBase, &cb, sizeof(cb));
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, 0, cl);
        gfx.SetComputeDescriptorTable(kSRV0, depthSrvHandle, cl);
        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_texture, 0), cl);

        gfx.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_texture), cl);
    }

    // ---- Mips 1..N-1: UAV→UAV reduce ---------------------------------------
    gfx.BindComputePipelineState(m_reducePSO, cl);

    const uint32_t maxMips = (std::min)(m_mipCount, kMaxMipsCB);
    for (uint32_t mip = 1; mip < maxMips; ++mip)
    {
        const uint32_t srcW = (std::max)(m_width  >> (mip - 1), 1u);
        const uint32_t srcH = (std::max)(m_height >> (mip - 1), 1u);
        const uint32_t dstW = (std::max)(m_width  >> mip, 1u);
        const uint32_t dstH = (std::max)(m_height >> mip, 1u);

        const uint32_t cbOffset = mip * kCBStride;
        HierCB cb{ srcW, srcH, dstW, dstH };
        std::memcpy(cbBase + cbOffset, &cb, sizeof(cb));
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, cbOffset, cl);

        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_texture, mip - 1), cl);
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureMipUAVGpuHandle(m_texture, mip), cl);

        gfx.DispatchCompute((dstW + 7) / 8, (dstH + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_texture), cl);
    }
}
