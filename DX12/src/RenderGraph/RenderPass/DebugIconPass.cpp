#include "RenderGraph/RenderPass/DebugIconPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>

using namespace DirectX;

// Engine-shared root signature slots (identical to WorldUIBillboardPass):
//   b1 space0 — PerView CB (we upload our own un-jittered viewProj here)
//   t2 space0 — vertex ByteAddressBuffer
//   t0 space2 — bindless Texture2D[] table
//   s0        — linear-clamp sampler
static constexpr uint32_t kRootSlot_VertexSRV   = 10; // t2 space0
static constexpr uint32_t kRootSlot_BindlessTex = 27; // t0 space2 (engine-shared)

void DebugIconPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // Reuse the WorldUI billboard shaders — same vertex layout + root slots.
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::WorldUI_VS, RHI::ShaderStage::VS, "WorldUI.vs.hlsl");
    m_shaderLib.Register(ShaderID::WorldUI_PS, RHI::ShaderStage::PS, "WorldUI.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxVertices) * sizeof(IconVertex);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_vertexBuffer[i]))
                m_vbMapped[i] = gfx.MapBuffer(m_vertexBuffer[i]);
            else
                LOG_ERROR("DebugIconPass: VB[%u] creation failed", i);
        }
    }

    m_cb.Create(gfx, "DebugIcon.CB");

    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        if (!gfx.CreateSampler(sd, m_samplerIdx))
            LOG_ERROR("DebugIconPass: sampler creation failed");
    }

    if (m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_SUCCESS("DebugIconPass: initialized");
    else
        LOG_ERROR("DebugIconPass: PSO creation failed");
}

PSODesc DebugIconPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::WorldUI_VS;
    desc.psID        = ShaderID::WorldUI_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::TRIANGLELIST;

    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = false;

    // Always-on-top overlay — depth off so icons read clearly even behind
    // geometry (matches the "overlay" gizmo convention).
    desc.dss.depth_enable     = false;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;

    auto& rt = desc.bs.render_target[0];
    rt.blend_enable             = true;
    rt.src_blend                = RHI::Blend::SRC_ALPHA;
    rt.dest_blend               = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op                 = RHI::BlendOp::ADD;
    rt.src_blend_alpha          = RHI::Blend::ONE;
    rt.dest_blend_alpha         = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op_alpha           = RHI::BlendOp::ADD;
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R8G8B8A8_UNORM;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;
    return desc;
}

void DebugIconPass::AddIcon(const XMFLOAT3& worldPos, float halfSize,
                            uint32_t texIdx, uint32_t color)
{
    m_icons.push_back({ worldPos, halfSize, texIdx, color });
}

void DebugIconPass::Execute(RHI::CommandList cl,
                            const XMFLOAT4X4& viewProjMatrix,
                            const XMFLOAT4X4& viewMatrix,
                            const RHI::Texture* target,
                            RHI::ResourceState entryState,
                            uint32_t canvasW, uint32_t canvasH)
{
    // INVARIANT: every early-out below MUST precede the first resource barrier
    // (the SR→RT transition further down). That keeps this pass either a clean
    // no-op or a complete SR→RT→SR pair — never a half-transition that would
    // desync the caller's render-target state tracker. Don't add GPU
    // state-mutation before the precondition checks.
    if (!enabled || m_icons.empty() || !m_gfx || !target || !target->IsValid()) return;
    if (canvasW == 0 || canvasH == 0) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);
    const uint32_t frameSlot = gfx.GetFrameIndex();
    if (!m_vbMapped[frameSlot]) return;

    const PSODesc psoDesc = BuildPSODesc();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(psoDesc);
    if (!pso || !pso->IsValid()) return;

    // World-space camera axes = columns of the row-major DX view matrix.
    const float* vm = reinterpret_cast<const float*>(&viewMatrix);
    const XMFLOAT3 cameraRightWS{ vm[0], vm[4], vm[8]  };
    const XMFLOAT3 cameraUpWS   { vm[1], vm[5], vm[9]  };

    static thread_local std::vector<IconVertex> verts;
    verts.clear();
    verts.reserve(m_icons.size() * 6);

    const XMVECTOR right0 = XMLoadFloat3(&cameraRightWS);
    const XMVECTOR up0    = XMLoadFloat3(&cameraUpWS);

    for (const IconReq& ic : m_icons)
    {
        if (verts.size() + 6 > kMaxVertices) break;

        const XMVECTOR center = XMLoadFloat3(&ic.pos);
        const XMVECTOR right  = XMVectorScale(right0, ic.halfSize);
        const XMVECTOR up     = XMVectorScale(up0,    ic.halfSize);

        XMFLOAT3 p[4];
        XMStoreFloat3(&p[0], XMVectorSubtract(XMVectorAdd(center, up), right));     // top-L
        XMStoreFloat3(&p[1], XMVectorAdd(XMVectorAdd(center, up), right));          // top-R
        XMStoreFloat3(&p[2], XMVectorAdd(XMVectorSubtract(center, up), right));     // bot-R
        XMStoreFloat3(&p[3], XMVectorSubtract(XMVectorSubtract(center, up), right));// bot-L

        auto pushV = [&](const XMFLOAT3& xyz, float u, float v) {
            IconVertex vv{};
            vv.pos[0] = xyz.x; vv.pos[1] = xyz.y; vv.pos[2] = xyz.z;
            vv.uv[0]  = u;     vv.uv[1]  = v;
            vv.col    = ic.col;
            vv.texIdx = ic.texIdx;
            verts.push_back(vv);
        };
        // CCW, matching WorldUI EmitQuad winding (PSO is cull-NONE anyway).
        pushV(p[0], 0.f, 0.f);
        pushV(p[1], 1.f, 0.f);
        pushV(p[2], 1.f, 1.f);
        pushV(p[0], 0.f, 0.f);
        pushV(p[2], 1.f, 1.f);
        pushV(p[3], 0.f, 1.f);
    }

    if (verts.empty()) return;

    std::memcpy(m_vbMapped[frameSlot], verts.data(), verts.size() * sizeof(IconVertex));

    if (entryState != RHI::ResourceState::RENDERTARGET)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            target, entryState, RHI::ResourceState::RENDERTARGET), cl);
    }

    const RHI::Texture* rtArr[1] = { target };
    gfx.SetRenderTargets(1, rtArr, nullptr, cl);

    RHI::Viewport vp{};
    vp.top_left_x = 0; vp.top_left_y = 0;
    vp.width  = static_cast<float>(canvasW);
    vp.height = static_cast<float>(canvasH);
    vp.min_depth = 0.f; vp.max_depth = 1.f;
    gfx.SetViewport(vp, cl);
    gfx.SetScissorRect(0, 0, canvasW, canvasH, cl);

    gfx.BindDescriptorHeaps(cl);
    gfx.BindPipelineState(*pso, cl);
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, cl);

    // Un-jittered viewProj into our b1 stub (engine stores CB matrices transposed).
    if (auto* slot = m_cb.Current(gfx))
    {
        XMMATRIX m = XMLoadFloat4x4(&viewProjMatrix);
        XMStoreFloat4x4(slot, XMMatrixTranspose(m));
    }
    gfx.BindConstantBuffer(m_cb.CurrentBuffer(gfx), 0, cl);   // b1 space0
    if (m_samplerIdx >= 0) gfx.BindSampler(m_samplerIdx, 0, cl);

    const uint64_t vbHandle = gfx.GetBufferSRVGpuHandle(m_vertexBuffer[frameSlot]);
    if (vbHandle) gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbHandle, cl);

    if (const uint64_t bindlessTbl = gfx.GetBindlessTextureTableGpuHandle())
        gfx.BindDescriptorTableGpuHandle(kRootSlot_BindlessTex, bindlessTbl, cl);

    gfx.DrawInstanced(static_cast<uint32_t>(verts.size()), 1, 0, 0, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        target,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}

void DebugIconPass::ReloadShaders()
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
    m_psoCache.GetOrCreate(BuildPSODesc());
}
