#include "RenderGraph/RenderPass/SpotShadowPass.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cstring>

using namespace DirectX;

// Root parameter slots (must match GraphicsDX12's default graphics root sig).
static constexpr uint32_t kShadowPerViewSlot = 0;   // CB b1 space0 → BindCBByName index 0
static constexpr uint32_t kInstanceBufSlot   = 8;   // root SRV t0 space0 (unused here; indirect draw handles it)
static constexpr uint32_t kMeshDescSlot      = 9;   // root SRV t1 space0
static constexpr uint32_t kBindlessSlot      = 14;  // desc table bindless g_Buffers[]

// ---------------------------------------------------------------------------
SpotShadowPass::SpotShadowPass()
{
    for (uint32_t i = 0; i < kMaxCasters; ++i)
        XMStoreFloat4x4(&m_pendingVP[i], XMMatrixIdentity());
}

SpotShadowPass::~SpotShadowPass()
{
    if (!m_gfxPtr) return;
    for (uint32_t i = 0; i < kMaxCasters; ++i)
    {
        if (m_casterCBMapped[i]) m_gfxPtr->UnmapBuffer(m_casterCBs[i]);
        if (m_casterCBs[i].IsValid()) m_gfxPtr->DestroyBuffer(m_casterCBs[i]);
    }
    if (m_indirectArgMapped) m_gfxPtr->UnmapBuffer(m_indirectArgBuffer);
    if (m_indirectArgBuffer.IsValid()) m_gfxPtr->DestroyBuffer(m_indirectArgBuffer);
    if (m_atlas.IsValid()) m_gfxPtr->DestroyTexture(m_atlas);
}

// ---------------------------------------------------------------------------
void SpotShadowPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    // Reuse the CSM shadow shaders — they already write depth-only with a
    // single ShadowPerViewCB at b1 space0, exactly what we need.
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Shadow_VS, RHI::ShaderStage::VS, "Shadow.vs.hlsl");
    m_shaderLib.Register(ShaderID::Shadow_PS, RHI::ShaderStage::PS, "Shadow.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    PermutationKey basePerm;
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm)))
        LOG_ERROR("SpotShadowPass: base PSO creation failed");

    // ---- Shadow atlas: Texture2DArray<D32_FLOAT>, per-slice DSVs auto-created
    {
        RHI::TextureDesc td;
        td.width      = kShadowMapSize;
        td.height     = kShadowMapSize;
        td.format     = RHI::Format::D32_FLOAT;
        td.bind_flags = RHI::BindFlag::DEPTH_STENCIL | RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.mip_levels = 1;
        td.array_size = kMaxCasters;
        td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0); // reversed Z
        td.debug_name = "SpotShadowPass.Atlas";
        if (!gfx.CreateTexture(td, m_atlas))
            LOG_ERROR("SpotShadowPass: shadow atlas creation failed");
    }

    // ---- One tiny UPLOAD CB per slice (single float4x4 each) --------------
    for (uint32_t i = 0; i < kMaxCasters; ++i)
    {
        RHI::GPUBufferDesc bd;
        bd.size       = (sizeof(XMFLOAT4X4) + 255u) & ~255ull; // 256 bytes aligned
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_casterCBs[i]))
            m_casterCBMapped[i] = gfx.MapBuffer(m_casterCBs[i]);
        else
            LOG_ERROR("SpotShadowPass: caster CB %u creation failed", i);
    }

    // ---- Indirect draw command buffer (same layout as ShadowPass / GBuffer)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxIndirectCommands) * sizeof(IndirectDrawCommand);
        bd.stride     = sizeof(IndirectDrawCommand);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::NONE;
        if (gfx.CreateBuffer(bd, m_indirectArgBuffer))
            m_indirectArgMapped = gfx.MapBuffer(m_indirectArgBuffer);
    }

    // Linear-wrap sampler for alpha-test PS (matches GBuffer.ps's g_LinearWrap).
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::WRAP;
        sd.address_v = RHI::TextureAddressMode::WRAP;
        sd.address_w = RHI::TextureAddressMode::WRAP;
        if (!gfx.CreateSampler(sd, m_alphaSamplerIdx))
            LOG_ERROR("SpotShadowPass: alpha-test sampler creation failed");
    }

    LOG_SUCCESS("SpotShadowPass: initialised (Atlas %ux%u x %u slices, indirect=%s)",
                kShadowMapSize, kShadowMapSize, kMaxCasters,
                m_indirectArgMapped ? "YES" : "NO");
}

// ---------------------------------------------------------------------------
PSODesc SpotShadowPass::BuildPSODesc(PermutationKey perm) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::Shadow_VS;
    desc.psID        = ShaderID::Shadow_PS;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;  // PVF

	desc.rs.cull_mode = RHI::CullMode::NONE; // front-face cull → less acne //Use CullMode::None to prevent thin object cast light leaks.  Back-face cull can be used if the content is mostly closed meshes, but it causes more acne and doesn't eliminate all leaks.
    desc.rs.depth_clip_enable = false;

    // Slope-scale + constant bias (reversed-Z, D32_FLOAT, GREATER_EQUAL).
    // Negative values push the CASTER's stored depth AWAY from the light so
    // flat receivers don't self-shadow. Keep the slope-scaled term (adaptsㄒㄧㄢ
    // automatically at grazing angles) and use ONLY a minimal constant.
    //
    // The previous -100 constant + Lighting.ps +0.0015 receiver bias stacked
    // in the same direction — both biased toward "lit" — and over-lit
    // receivers just behind a thin occluder, producing visible light leaks
    // past walls (the bug this comment replaces). Convention is to pick one
    // side of the bias; we keep it all on the rasterizer and drop the
    // receiver-side bias to zero (see Lighting.ps SampleSpotShadow).
    desc.rs.depth_bias              = -1;
    desc.rs.slope_scaled_depth_bias = -1.0f;
    desc.rs.depth_bias_clamp        = 0.0f;

    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ALL;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.rtvCount  = 0;
    desc.dsvFormat = RHI::Format::D32_FLOAT;
    return desc;
}

// ---------------------------------------------------------------------------
uint64_t SpotShadowPass::GetAtlasSrvHandle() const
{
    if (!m_gfxPtr || !m_atlas.IsValid()) return 0;
    return m_gfxPtr->GetTextureSRVGpuHandle(m_atlas);
}

// ---------------------------------------------------------------------------
void SpotShadowPass::UploadCasterCBs()
{
    for (uint32_t i = 0; i < m_activeCount; ++i)
    {
        if (!m_casterCBMapped[i]) continue;
        XMMATRIX m = XMLoadFloat4x4(&m_pendingVP[i]);
        XMFLOAT4X4 t;
        XMStoreFloat4x4(&t, XMMatrixTranspose(m));
        std::memcpy(m_casterCBMapped[i], &t, sizeof(XMFLOAT4X4));
    }
}

// ---------------------------------------------------------------------------
RHI::CommandList SpotShadowPass::Execute(RHI::CommandList cl)
{
    if (!m_atlas.IsValid()) return cl;

    // No shadow-casting spot lights this frame → skip the atlas clear pass
    // entirely. Previously we were running 8 depth-slice clears + 2 barriers
    // every frame even when no spot light had castsShadow on. The first
    // frame without casters still needs a one-shot DEPTHSTENCIL → SRV
    // transition so LightingPass can sample the zero-initialised atlas
    // (reversed Z → "no shadow" at depth=0 after the clear value in Init);
    // after that we stay in SRV state until a caster reappears.
    if (m_activeCount == 0)
    {
        if (m_firstExecution)
        {
            cl.PushBarrier(RHI::GPUBarrier::Image(
                &m_atlas,
                RHI::ResourceState::DEPTHSTENCIL,
                RHI::ResourceState::DEPTH_READ_SRV));
            m_firstExecution = false;
        }
        return cl;
    }

    IGraphicsDevice& gfx = *m_gfxPtr;

    // On subsequent frames the atlas arrives in DEPTH_READ_SRV (Lighting.ps
    // sampled it last frame). Transition back to DEPTHSTENCIL for writes.
    if (!m_firstExecution)
    {
        cl.PushBarrier(RHI::GPUBarrier::Image(
            &m_atlas,
            RHI::ResourceState::DEPTH_READ_SRV,
            RHI::ResourceState::DEPTHSTENCIL));
    }

    // Always clear every slice — stale depth from last frame would corrupt
    // the compare for a slice whose caster list changed.
    for (uint32_t s = 0; s < kMaxCasters; ++s)
        gfx.ClearDepthStencilSlice(m_atlas, s, 0.0f, 0, cl); // reversed Z

    // Pull the draw lists we need ONCE.
    DrawList opaqueDraws      = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    DrawList shadowDraws      = cl.GetContext().GetDrawList(DrawFilter::Shadow);
    // Alpha-tested transparents (foliage with BlendMode::Alpha + alphaRef).
    DrawList transparentDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);

    if (m_activeCount == 0 ||
        (opaqueDraws.empty() && shadowDraws.empty() && transparentDraws.empty()))
    {
        // No renderable content — transition back and bail.
        cl.PushBarrier(RHI::GPUBarrier::Image(
            &m_atlas,
            RHI::ResourceState::DEPTHSTENCIL,
            RHI::ResourceState::DEPTH_READ_SRV));
        m_firstExecution = false;
        return cl;
    }

    UploadCasterCBs();

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const bool useIndirect = (m_indirectArgMapped != nullptr);

    // Build indirect arg buffer, split alpha / non-alpha — same layout as CSM.
    uint32_t groupOffsets[2] = { 0, 0 };
    uint32_t groupCounts[2]  = { 0, 0 };

    if (useIndirect)
    {
        auto* args = static_cast<IndirectDrawCommand*>(m_indirectArgMapped);

        // Transparent source contributes ONLY to the alpha-test bucket — pure
        // alpha-blended packets (no alphaRef) would otherwise render solid.
        auto countAlpha = [&](DrawList list, bool wantAlpha, bool transparentSrc) -> uint32_t {
            uint32_t n = 0;
            for (const DrawPacket& dp : list)
            {
                if (transparentSrc && !dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;
                if (dp.permutation.Has(PermutationKey::ALPHA_TEST) == wantAlpha)
                    ++n;
            }
            return n;
        };
        groupCounts[0] = countAlpha(opaqueDraws, false, false) + countAlpha(shadowDraws, false, false);
        groupCounts[1] = countAlpha(opaqueDraws, true,  false) + countAlpha(shadowDraws, true,  false)
                       + countAlpha(transparentDraws, true, true);

        groupOffsets[0] = 0;
        groupOffsets[1] = groupCounts[0] * sizeof(IndirectDrawCommand);

        uint32_t writeIdx = 0;
        auto writeGroup = [&](DrawList list, bool wantAlpha, bool transparentSrc) {
            for (const DrawPacket& dp : list)
            {
                if (transparentSrc && !dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;
                bool isAlpha = dp.permutation.Has(PermutationKey::ALPHA_TEST);
                if (isAlpha != wantAlpha) continue;
                if (writeIdx >= kMaxIndirectCommands) break;
                auto& ic = args[writeIdx++];
                ic.meshDescIdx     = dp.meshDescriptorIndex;
                ic.instanceOffset  = dp.instanceOffset;
                ic.materialIndex   = dp.materialIndex;
                ic.prevPosInfo     = 0;
                ic.vertexCountPerInstance = dp.vertexOrIndexCount;
                ic.instanceCount          = dp.instanceCount;
                ic.startVertexLocation    = 0;
                ic.startInstanceLocation  = 0;
            }
        };
        writeGroup(opaqueDraws,      false, false);
        writeGroup(shadowDraws,      false, false);
        writeGroup(opaqueDraws,      true,  false);
        writeGroup(shadowDraws,      true,  false);
        writeGroup(transparentDraws, true,  true);
    }

    // ---- Render each active caster into its slice --------------------------
    for (uint32_t s = 0; s < m_activeCount; ++s)
    {
        if (!m_casterCBs[s].IsValid()) continue;

        gfx.SetDepthStencilSlice(m_atlas, s, cl);

        cl.SetViewport(kShadowMapSize, kShadowMapSize);
        cl.SetScissorRect(kShadowMapSize, kShadowMapSize);
        cl.SetPrimitiveTopology();

        for (int g = 0; g < 2; ++g)
        {
            if (useIndirect && groupCounts[g] == 0) continue;

            PermutationKey perm;
            if (g == 1) perm.Set(PermutationKey::ALPHA_TEST, true);

            const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc(perm));
            if (!pso || !pso->IsValid()) continue;

            cl.SetPipelineState(*pso);
            cl.BindDescriptorHeaps();
            cl.BindBufferSRVByName(kInstanceBufSlot, "InstanceBuffer");
            cl.BindBufferSRVByName(kMeshDescSlot,    "MeshDescriptors");
            if (bindlessHandle)
                cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
            cl.GetDevice().BindConstantBuffer(m_casterCBs[s],
                                              kShadowPerViewSlot, cl);

            // ALPHA_TEST PS reads MaterialBuffer (alphaRef) + bindless g_AllTextures[].
            if (g == 1)
            {
                if (const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer("MaterialBuffer"))
                    if (matBuf->IsValid())
                        cl.BindBufferSRV(0, *matBuf);
                if (cl.gfx)
                {
                    auto& dx12 = static_cast<GraphicsDX12&>(*cl.gfx);
                    D3D12_GPU_DESCRIPTOR_HANDLE texTable = dx12.GetBindlessTextureTableHandle();
                    if (texTable.ptr)
                        cl.BindDescriptorTableHandle(27, texTable.ptr);
                }
                if (m_alphaSamplerIdx >= 0)
                    cl.BindSampler(0, m_alphaSamplerIdx);
            }

            if (useIndirect)
            {
                gfx.ExecuteIndirectDraw(m_indirectArgBuffer,
                                        groupOffsets[g],
                                        groupCounts[g],
                                        nullptr, 0, cl);
            }
            else
            {
                auto fallbackDraw = [&](DrawList list, bool transparentSrc) {
                    for (const DrawPacket& dp : list)
                    {
                        if (transparentSrc && !dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;
                        bool isAlpha = dp.permutation.Has(PermutationKey::ALPHA_TEST);
                        if ((g == 0 && isAlpha) || (g == 1 && !isAlpha)) continue;
                        cl.SetPVFRootConstants(dp.meshDescriptorIndex,
                                               dp.instanceOffset,
                                               dp.materialIndex);
                        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
                    }
                };
                fallbackDraw(opaqueDraws,      false);
                fallbackDraw(shadowDraws,      false);
                if (g == 1) fallbackDraw(transparentDraws, true);
            }
        }
    }

    cl.PushBarrier(RHI::GPUBarrier::Image(
        &m_atlas,
        RHI::ResourceState::DEPTHSTENCIL,
        RHI::ResourceState::DEPTH_READ_SRV));

    m_firstExecution = false;
    return cl;
}
