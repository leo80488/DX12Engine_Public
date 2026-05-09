#include "RenderGraph/RenderPass/SceneColorPyramidPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

namespace
{
    constexpr uint32_t kCBSlot = 0;  // b0 space2
    constexpr uint32_t kSRV0   = 1;  // t0 space2 — HDR snapshot (mip 0 path)
    constexpr uint32_t kUAV0   = 4;  // u0 space2 — mip0 dst / reduce src
    constexpr uint32_t kUAV1   = 5;  // u1 space2 — reduce dst
}

void SceneColorPyramidPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SceneColorPyramidMip0_CS, RHI::ShaderStage::CS,
                         "SceneColorPyramid.cs.hlsl", "CSMain_Mip0");
    m_shaderLib.Register(ShaderID::SceneColorPyramidReduce_CS, RHI::ShaderStage::CS,
                         "SceneColorPyramid.cs.hlsl", "CSMain_Reduce");

    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SceneColorPyramidMip0_CS);
        if (!cs) { LOG_ERROR("SceneColorPyramidPass: mip0 CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_mip0PSO))
        { LOG_ERROR("SceneColorPyramidPass: mip0 PSO create failed"); return; }
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SceneColorPyramidReduce_CS);
        if (!cs) { LOG_ERROR("SceneColorPyramidPass: reduce CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_reducePSO))
        { LOG_ERROR("SceneColorPyramidPass: reduce PSO create failed"); return; }
    }

    RHI::GPUBufferDesc bd{};
    bd.size       = 256;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    LOG_SUCCESS("SceneColorPyramidPass: initialized");
}

void SceneColorPyramidPass::EnsureTexture(uint32_t w, uint32_t h)
{
    if (!m_gfx || w == 0 || h == 0) return;
    if (w == m_width && h == m_height && m_texture.IsValid()) return;

    if (m_texture.IsValid()) m_gfx->DestroyTexture(m_texture);
    m_width  = w;
    m_height = h;

    m_mipCount = 1;
    { uint32_t dim = std::max(w, h); while (dim > 1) { dim >>= 1; m_mipCount++; } }

    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.mip_levels = m_mipCount;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, m_texture))
        LOG_ERROR("SceneColorPyramidPass: pyramid create failed (%ux%u, %u mips)",
                  w, h, m_mipCount);
    else
        LOG_INFO("SceneColorPyramidPass: pyramid %ux%u (%u mips) ready",
                 w, h, m_mipCount);
}

uint64_t SceneColorPyramidPass::GetSrvHandle() const
{
    if (!m_gfx || !m_texture.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_texture);
}

void SceneColorPyramidPass::Execute(RHI::CommandList cl, const RHI::Texture* snapshotTex)
{
    if (!m_reducePSO.IsValid()) return;
    if (!m_texture.IsValid() || m_mipCount == 0 || !m_cbMapped) return;
    if (!snapshotTex || !snapshotTex->IsValid()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Mip 0: D3D12 native copy snapshot → pyramid mip 0.
    //   The CS-based path (mip0 PSO) silently dropped writes — both per-mip
    //   UAV (mipUavs[0]) and whole-resource UAV (entry.uav) bindings showed
    //   the same symptom: dispatch ran without error but the resource never
    //   reflected the writes. SSRDepthHierarchyPass uses the same code
    //   pattern and works (R16G16 format), so the issue is specific to this
    //   pyramid (R16G16B16A16_FLOAT, 11 mips). Workaround: skip CS, use
    //   CopyTextureSubresource which is a hardware copy engine path with
    //   different state-tracking requirements.
    //
    // State transitions: snapshot SR → COPY_SRC, pyramid (whole) UAV →
    // COPY_DEST, copy, snapshot back to SR, pyramid back to UAV (so the
    // reduce loop below can read mip 0 as a UAV).
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        snapshotTex, RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::COPY_SRC), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_texture, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::COPY_DST), cl);

    gfx.CopyTextureSubresource(*snapshotTex, /*srcMip*/0, /*srcSlice*/0,
                                m_texture,    /*dstMip*/0, /*dstSlice*/0, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        snapshotTex, RHI::ResourceState::COPY_SRC,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_texture, RHI::ResourceState::COPY_DST,
        RHI::ResourceState::UNORDERED_ACCESS), cl);

    // Mips 1..N-1: UAV→UAV reduce (Karis firefly-weighted average).
    gfx.BindComputePipelineState(m_reducePSO, cl);
    for (uint32_t mip = 1; mip < m_mipCount; ++mip)
    {
        const uint32_t srcW = std::max(m_width  >> (mip - 1), 1u);
        const uint32_t srcH = std::max(m_height >> (mip - 1), 1u);
        const uint32_t dstW = std::max(m_width  >> mip, 1u);
        const uint32_t dstH = std::max(m_height >> mip, 1u);

        HierCB cb{ srcW, srcH, dstW, dstH };
        std::memcpy(m_cbMapped, &cb, sizeof(cb));
        gfx.SetComputeRootCBV(kCBSlot, m_cb, cl);

        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_texture, mip - 1), cl);
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureMipUAVGpuHandle(m_texture, mip), cl);

        gfx.DispatchCompute((dstW + 7) / 8, (dstH + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_texture), cl);
    }
}
