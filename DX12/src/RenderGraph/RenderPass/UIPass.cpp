#include "RenderGraph/RenderPass/UIPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <algorithm>
#include <cstring>

// Engine default root signature slots (mirrors DebugWirePass usage):
//   BindConstantBuffer(slot=0)              → root param 1   (b1 space0)
//   BindDescriptorTableGpuHandle(rootSlot)  → 10..13 = t2..t5 space0
//                                          17 = sampler s2  (here we use s0 = 15)
static constexpr uint32_t kRootSlot_VertexSRV  = 10; // t2 space0
static constexpr uint32_t kRootSlot_TextureSRV = 11; // t3 space0

void UIPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::UI_VS, RHI::ShaderStage::VS, "UI.vs.hlsl");
    m_shaderLib.Register(ShaderID::UI_PS, RHI::ShaderStage::PS, "UI.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    // Vertex buffer — sized for `kMaxVertices` flat-expanded triangle verts.
    // (UIDrawList stores v+idx; UIPass expands to flat triangle list at upload
    // time because the engine's PVF root signature does not expose IA index
    // buffer binding — see GraphicsDX12::IASetIndexBuffer(nullptr).)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxIndices) * sizeof(UI::UIVertex);
        bd.stride     = 0;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (gfx.CreateBuffer(bd, m_vertexBuffer))
            m_vbMapped = gfx.MapBuffer(m_vertexBuffer);
        else
            LOG_ERROR("UIPass: vertex buffer creation failed");
    }

    // UI CB (canvas size at b1 space0).
    {
        RHI::GPUBufferDesc bd{};
        struct UICB { float w, h; uint32_t pad0, pad1; };
        bd.size       = (sizeof(UICB) + 255) & ~255u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_cb))
            m_cbMapped = gfx.MapBuffer(m_cb);
    }

    // 1×1 white default texture — bound when a draw cmd has no texture so the
    // PS branch stays unified (sample × tint = tint when sampling white).
    if (!CreateWhiteTexture())
        LOG_ERROR("UIPass: white texture creation failed");

    // Linear/clamp sampler at s0.
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        if (!gfx.CreateSampler(sd, m_samplerIdx))
            LOG_ERROR("UIPass: sampler creation failed");
    }

    if (m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_SUCCESS("UIPass: initialized (max %u verts, %u indices, white tex SRV=%llu)",
                    kMaxVertices, kMaxIndices,
                    static_cast<unsigned long long>(m_whiteTexSrv));
    else
        LOG_ERROR("UIPass: PSO creation failed");
}

bool UIPass::CreateWhiteTexture()
{
    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
    td.width      = 1;
    td.height     = 1;
    td.depth      = 1;
    td.format     = RHI::Format::R8G8B8A8_UNORM;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;

    static const uint8_t kWhitePx[4] = { 255, 255, 255, 255 };
    RHI::SubresourceData sub{};
    sub.data_ptr    = kWhitePx;
    sub.row_pitch   = 4;
    sub.slice_pitch = 4;

    if (!m_gfx->CreateTexture(td, m_whiteTex, &sub)) return false;
    m_whiteTexSrv = m_gfx->GetTextureSRVGpuHandle(m_whiteTex);
    return m_whiteTexSrv != 0;
}

PSODesc UIPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::UI_VS;
    desc.psID        = ShaderID::UI_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::TRIANGLELIST;

    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = false;

    // No depth — UI draws on top of everything.
    desc.dss.depth_enable     = false;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;

    // Standard alpha-over blend.
    auto& rt = desc.bs.render_target[0];
    rt.blend_enable             = true;
    rt.src_blend                = RHI::Blend::SRC_ALPHA;
    rt.dest_blend               = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op                 = RHI::BlendOp::ADD;
    rt.src_blend_alpha          = RHI::Blend::ONE;
    rt.dest_blend_alpha         = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op_alpha           = RHI::BlendOp::ADD;
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    // ToneMap final-output target format (RGBA8 UNORM).
    desc.rtvFormats[0] = RHI::Format::R8G8B8A8_UNORM;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;

    return desc;
}

void UIPass::Execute(RHI::CommandList cl,
                     const RHI::Texture* target,
                     RHI::ResourceState entryState,
                     uint32_t canvasW, uint32_t canvasH)
{
    if (!enabled || !m_gfx || !target || !target->IsValid()) return;
    if (m_drawList.IsEmpty()) return;
    if (canvasW == 0 || canvasH == 0) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);
    const PSODesc psoDesc = BuildPSODesc();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(psoDesc);
    if (!pso || !pso->IsValid()) return;
    if (!m_vbMapped) return;

    // ---- Upload: flat-expand UIDrawList → triangle list VB --------------
    // UIDrawList stores indexed primitives. The engine root sig has no IA
    // index buffer slot (PVF model), so we walk the index list and copy the
    // referenced vertices in order. Each command becomes a contiguous
    // [vertOffset, vertCount) range we DrawInstanced over.
    const auto& srcVerts = m_drawList.Vertices();
    const auto& srcIdx   = m_drawList.Indices();
    const auto& srcCmds  = m_drawList.Commands();

    if (srcIdx.size() > kMaxIndices)
    {
        LOG_WARNING("UIPass: drawlist index count %zu exceeds kMaxIndices %u — truncating",
                    srcIdx.size(), kMaxIndices);
    }
    const size_t totalVerts = std::min<size_t>(srcIdx.size(), kMaxIndices);
    UI::UIVertex* dst = static_cast<UI::UIVertex*>(m_vbMapped);

    struct ExpandedCmd
    {
        uint32_t          vertexOffset;
        uint32_t          vertexCount;
        UI::Rect          clipRect;
        uint64_t          texSrv;
    };
    std::vector<ExpandedCmd> expanded;
    expanded.reserve(srcCmds.size());

    uint32_t writeOff = 0;
    for (const auto& cmd : srcCmds)
    {
        if (cmd.indexCount == 0) continue;
        const uint32_t startIdx = cmd.indexOffset;
        const uint32_t endIdx   = startIdx + cmd.indexCount;
        const uint32_t expandStart = writeOff;
        for (uint32_t i = startIdx; i < endIdx; ++i)
        {
            if (writeOff >= kMaxIndices) break;
            const uint16_t vi = srcIdx[i];
            if (vi < srcVerts.size())
                dst[writeOff] = srcVerts[vi];
            ++writeOff;
        }
        const uint32_t expandCount = writeOff - expandStart;
        if (expandCount == 0) continue;
        expanded.push_back({
            expandStart, expandCount, cmd.clipRect,
            cmd.texture.srvGpuHandle ? cmd.texture.srvGpuHandle : m_whiteTexSrv
        });
        if (writeOff >= kMaxIndices) break;
    }
    if (expanded.empty()) return;
    (void)totalVerts;

    // ---- Update CB (canvas size) ----------------------------------------
    if (m_cbMapped)
    {
        struct UICB { float w, h; uint32_t pad0, pad1; };
        UICB cb{ static_cast<float>(canvasW), static_cast<float>(canvasH), 0, 0 };
        std::memcpy(m_cbMapped, &cb, sizeof(cb));
    }

    // ---- Transition target → RT ----------------------------------
    // Caller passes the actual tracked state of the target (typically
    // SHADER_RESOURCE after ToneMapPass::TransitionForDisplay, or
    // UNORDERED_ACCESS / SHADER_RESOURCE_COMPUTE if other paths skipped
    // that step). Issue the matching barrier — using a hard-coded entry
    // state here triggered D3D12 validation when the resize path left the
    // texture in UAV instead of SR.
    if (entryState != RHI::ResourceState::RENDERTARGET)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            target,
            entryState,
            RHI::ResourceState::RENDERTARGET), cl);
    }

    // ---- Bind RTV (no clear — preserve tonemapped scene) ---------------
    const RHI::Texture* rtArr[1] = { target };
    gfx.SetRenderTargets(1, rtArr, nullptr, cl);

    RHI::Viewport vp{};
    vp.top_left_x = 0;
    vp.top_left_y = 0;
    vp.width      = static_cast<float>(canvasW);
    vp.height     = static_cast<float>(canvasH);
    vp.min_depth  = 0.0f;
    vp.max_depth  = 1.0f;
    gfx.SetViewport(vp, cl);

    gfx.BindDescriptorHeaps(cl);
    gfx.BindPipelineState(*pso, cl);
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, cl);

    // CB at b1 space0 — engine helper: slot=0 → b1.
    gfx.BindConstantBuffer(m_cb, 0, cl);
    // Sampler at s0 — engine helper: slot=0 → root param 15.
    if (m_samplerIdx >= 0) gfx.BindSampler(m_samplerIdx, 0, cl);

    // VB ByteAddressBuffer at t2 space0.
    const uint64_t vbHandle = gfx.GetBufferSRVGpuHandle(m_vertexBuffer);
    if (vbHandle) gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbHandle, cl);

    // ---- Draw, switching texture + scissor per command ------------------
    // Diagnostic — emit ONCE per change in cmd count to spot anomalies
    // without filling DX12Log.txt with thousands of identical lines.
    static size_t s_lastCmdCount = SIZE_MAX;
    const bool uiPassLog = (expanded.size() != s_lastCmdCount);
    if (uiPassLog) {
        s_lastCmdCount = expanded.size();
        LOG_INFO("UIPass.Execute: %zu draws (canvas %ux%u, whiteSrv=%llu)",
                  expanded.size(), canvasW, canvasH,
                  static_cast<unsigned long long>(m_whiteTexSrv));
    }

    uint64_t lastTex = 0;
    int cmdIdx = 0;
    for (const auto& ecmd : expanded)
    {
        const uint32_t l = static_cast<uint32_t>(std::max(0.f, ecmd.clipRect.mn.x));
        const uint32_t t = static_cast<uint32_t>(std::max(0.f, ecmd.clipRect.mn.y));
        const uint32_t r = static_cast<uint32_t>(std::min<float>(static_cast<float>(canvasW),
                                                                  ecmd.clipRect.mx.x));
        const uint32_t b = static_cast<uint32_t>(std::min<float>(static_cast<float>(canvasH),
                                                                  ecmd.clipRect.mx.y));
        if (r <= l || b <= t) {
            if (uiPassLog)
                LOG_WARNING("UIPass.cmd[%d]: SKIP clip empty (clip=(%.0f,%.0f,%.0f,%.0f) "
                             "→ scissor=(%u,%u,%u,%u))", cmdIdx,
                             ecmd.clipRect.mn.x, ecmd.clipRect.mn.y,
                             ecmd.clipRect.mx.x, ecmd.clipRect.mx.y, l, t, r, b);
            ++cmdIdx;
            continue; // fully clipped
        }
        gfx.SetScissorRect(l, t, r, b, cl);

        if (ecmd.texSrv != lastTex)
        {
            gfx.BindDescriptorTableGpuHandle(kRootSlot_TextureSRV, ecmd.texSrv, cl);
            lastTex = ecmd.texSrv;
        }
        gfx.DrawInstanced(ecmd.vertexCount, 1, ecmd.vertexOffset, 0, cl);
        if (uiPassLog) {
            // Dump first 4 vertex positions for this cmd so we can correlate
            // with screen-space expectations. Each vertex is 20 bytes; we
            // wrote into the same buffer that's about to be drawn.
            const UI::UIVertex* v = static_cast<UI::UIVertex*>(m_vbMapped) + ecmd.vertexOffset;
            const uint32_t showCount = std::min<uint32_t>(4u, ecmd.vertexCount);
            char vertBuf[256] = {};
            int off = 0;
            for (uint32_t k = 0; k < showCount; ++k) {
                const int n = std::snprintf(vertBuf + off, sizeof(vertBuf) - off,
                                             " v%u=(%.0f,%.0f)", k, v[k].pos[0], v[k].pos[1]);
                if (n <= 0 || off + n >= (int)sizeof(vertBuf)) break;
                off += n;
            }
            LOG_INFO("UIPass.cmd[%d]: DRAW vOff=%u vCount=%u tex=%llu scissor=(%u,%u,%u,%u)%s",
                      cmdIdx, ecmd.vertexOffset, ecmd.vertexCount,
                      static_cast<unsigned long long>(ecmd.texSrv),
                      l, t, r, b, vertBuf);
        }
        ++cmdIdx;
    }

    // ---- Transition target back to SR for downstream composite/ImGui ---
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        target,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}

void UIPass::ReloadShaders()
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
}
