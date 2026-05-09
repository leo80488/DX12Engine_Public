#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>
#include <cmath>

// Compute root signature slots (shared with all compute passes, space2).
static constexpr uint32_t kCBSlot     = 0;   // b0 space2
static constexpr uint32_t kSRV0       = 1;   // t0 space2
static constexpr uint32_t kSRV1       = 2;   // t1 space2
static constexpr uint32_t kSRV2       = 3;   // t2 space2 — XeGTAO Hilbert LUT (shared with
                                             //             skinned-mesh blend data; dispatches
                                             //             are mutually exclusive).
static constexpr uint32_t kUAV0       = 4;   // u0 space2
static constexpr uint32_t kUAV1       = 5;   // u1 space2
static constexpr uint32_t kUAV2       = 14;  // u2 space2 (XeGTAO prefilter mip 2)
static constexpr uint32_t kUAV3       = 15;  // u3 space2 (XeGTAO prefilter mip 3)
static constexpr uint32_t kUAV4       = 16;  // u4 space2 (XeGTAO prefilter mip 4)

// Intel XeGTAO Hilbert curve — verbatim port of XE_HILBERT_LEVEL=6 (64x64 pixels,
// 4096 cells). Used by SpatioTemporalNoise to drive an R2 low-discrepancy sequence
// from a spatially decorrelated base index — a much better noise pattern than the
// pure R2 we were using before.
static constexpr uint32_t XE_HILBERT_LEVEL = 6u;
static constexpr uint32_t XE_HILBERT_WIDTH = 1u << XE_HILBERT_LEVEL;  // 64
static constexpr uint32_t XE_HILBERT_AREA  = XE_HILBERT_WIDTH * XE_HILBERT_WIDTH; // 4096

static uint32_t HilbertIndex(uint32_t posX, uint32_t posY)
{
    uint32_t index = 0u;
    for (uint32_t curLevel = XE_HILBERT_WIDTH / 2u; curLevel > 0u; curLevel /= 2u)
    {
        const uint32_t regionX = (posX & curLevel) > 0u ? 1u : 0u;
        const uint32_t regionY = (posY & curLevel) > 0u ? 1u : 0u;
        index += curLevel * curLevel * ((3u * regionX) ^ regionY);
        if (regionY == 0u)
        {
            if (regionX == 1u)
            {
                posX = (XE_HILBERT_WIDTH - 1u) - posX;
                posY = (XE_HILBERT_WIDTH - 1u) - posY;
            }
            const uint32_t t = posX; posX = posY; posY = t;
        }
    }
    return index;
}

// CB layout inside a single shared upload buffer — offsets chosen so each
// dispatch binds its own 256-byte-aligned slice. Prevents the within-frame
// race where CPU overwrite before GPU reads would corrupt an earlier dispatch.
static constexpr uint32_t kCBOffsetMain      = 0;
static constexpr uint32_t kCBOffsetTemporal  = 256;
static constexpr uint32_t kCBOffsetDenoise   = 512;
static constexpr uint32_t kCBOffsetPrefilter = 768;  // prefilter[mip] at 768 + mip*256 (legacy: prefilter uses base only)
static constexpr uint32_t kCBTotalSize       = 256 * 8;  // 1 main + 1 temporal + 1 denoise + 5 prefilter

struct alignas(16) PrefilterCB
{
    uint32_t srcSize[2];
    uint32_t dstSize[2];
    float    depthUnpackConsts[2];
    float    effectRadius;
    float    effectFalloffRange;
    float    radiusMultiplier;
    uint32_t srcMip;
    float    _pad0[2];
};

// ---------------------------------------------------------------------------
void XeGTAOPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::XeGTAO_CS,              RHI::ShaderStage::CS,
                         "XeGTAO.cs.hlsl",                 "CSMain");
    m_shaderLib.Register(ShaderID::XeGTAODenoise_CS,       RHI::ShaderStage::CS,
                         "XeGTAODenoise.cs.hlsl",          "CSMain");
    m_shaderLib.Register(ShaderID::XeGTAODepthLinearize_CS, RHI::ShaderStage::CS,
                         "XeGTAODepthPrefilter.cs.hlsl",  "CSPrefilter");
    m_shaderLib.Register(ShaderID::XeGTAOTemporal_CS,      RHI::ShaderStage::CS,
                         "XeGTAOTemporal.cs.hlsl",         "CSMain");

    auto makeCS = [&](ShaderID id, RHI::PipelineState& out, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("XeGTAOPass: %s shader not found", tag); return; }
        RHI::PipelineStateDesc pd{};
        pd.cs = cs;
        if (!gfx.CreatePipelineState(pd, out))
            LOG_ERROR("XeGTAOPass: %s PSO creation failed", tag);
    };
    makeCS(ShaderID::XeGTAO_CS,               m_mainPSO,      "XeGTAO_CS");
    makeCS(ShaderID::XeGTAOTemporal_CS,       m_temporalPSO,  "XeGTAOTemporal_CS");
    makeCS(ShaderID::XeGTAODenoise_CS,        m_denoisePSO,   "XeGTAODenoise_CS");
    makeCS(ShaderID::XeGTAODepthLinearize_CS, m_prefilterPSO, "XeGTAODepthPrefilter_CS");

    // Shared CB — partitioned by byte offset per dispatch type (see kCBOffset*).
    RHI::GPUBufferDesc bd{};
    bd.size       = kCBTotalSize;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
    if (gfx.CreateBuffer(bd, m_cb))
        m_cbMapped = gfx.MapBuffer(m_cb);

    // ---- Hilbert LUT (64x64 R16_UINT, immutable) ---------------------------
    {
        std::vector<uint16_t> lut(XE_HILBERT_AREA);
        for (uint32_t y = 0; y < XE_HILBERT_WIDTH; ++y)
        for (uint32_t x = 0; x < XE_HILBERT_WIDTH; ++x)
        {
            const uint32_t idx = HilbertIndex(x, y);
            // idx range [0, 4096), always fits in uint16.
            lut[y * XE_HILBERT_WIDTH + x] = static_cast<uint16_t>(idx);
        }

        RHI::TextureDesc td{};
        td.width      = XE_HILBERT_WIDTH;
        td.height     = XE_HILBERT_WIDTH;
        td.format     = RHI::Format::R16_UINT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

        RHI::SubresourceData sub{};
        sub.data_ptr   = lut.data();
        sub.row_pitch  = XE_HILBERT_WIDTH * sizeof(uint16_t); // 128 bytes
        sub.slice_pitch = sub.row_pitch * XE_HILBERT_WIDTH;

        if (gfx.CreateTexture(td, m_hilbertLUT, &sub))
            m_hilbertLUTSrv = gfx.GetTextureSRVGpuHandle(m_hilbertLUT);
        else
            LOG_ERROR("XeGTAOPass: failed to create Hilbert LUT");
    }

    LOG_SUCCESS("XeGTAOPass: initialized");
}

// ---------------------------------------------------------------------------
void XeGTAOPass::SetViewportSize(uint32_t w, uint32_t h)
{
    if (w != m_vpW || h != m_vpH)
    {
        m_vpW = w;
        m_vpH = h;
        m_texDirty = true;
    }
}

// ---------------------------------------------------------------------------
void XeGTAOPass::SetProjectionMatrix(const DirectX::XMFLOAT4X4& projMatrix,
                                     const DirectX::XMFLOAT4X4& viewMatrix,
                                     uint32_t frameCounter)
{
    m_projMatrix   = projMatrix;
    m_viewMatrix   = viewMatrix;
    m_frameCounter = frameCounter;
}

// ---------------------------------------------------------------------------
void XeGTAOPass::RebuildTextures()
{
    if (!m_gfx || m_vpW == 0 || m_vpH == 0) return;

    auto createR8 = [&](RHI::Texture& tex, const char* tag)
    {
        if (tex.IsValid()) m_gfx->DestroyTexture(tex);
        RHI::TextureDesc td{};
        td.width      = m_vpW;
        td.height     = m_vpH;
        td.format     = RHI::Format::R8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!m_gfx->CreateTexture(td, tex))
            LOG_ERROR("XeGTAOPass: failed to create %s (%ux%u)", tag, m_vpW, m_vpH);
    };
    createR8(m_aoRaw,   "AO raw");
    createR8(m_aoFinal, "AO final");
    createR8(m_edges,   "edges");

    // Temporal ping-pong history (read index swaps each frame).
    createR8(m_aoHistory[0], "AO history[0]");
    createR8(m_aoHistory[1], "AO history[1]");
    m_aoHistoryState[0] = RHI::ResourceState::UNORDERED_ACCESS;
    m_aoHistoryState[1] = RHI::ResourceState::UNORDERED_ACCESS;
    m_historyWriteIdx   = 0;
    m_historyValid      = false;

    // Prev-frame linear-depth ping-pong (R32F, one mip). Allocated alongside
    // m_aoHistory so temporal can sample depth at the reprojected UV and
    // compare against current-frame depth for disocclusion detection.
    auto createR32 = [&](RHI::Texture& tex, const char* tag)
    {
        if (tex.IsValid()) m_gfx->DestroyTexture(tex);
        RHI::TextureDesc td{};
        td.width      = m_vpW;
        td.height     = m_vpH;
        td.format     = RHI::Format::R32_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!m_gfx->CreateTexture(td, tex))
            LOG_ERROR("XeGTAOPass: failed to create %s (%ux%u)", tag, m_vpW, m_vpH);
    };
    createR32(m_prevLinearDepth[0], "prev linear depth[0]");
    createR32(m_prevLinearDepth[1], "prev linear depth[1]");
    m_prevLinearDepthState[0] = RHI::ResourceState::UNORDERED_ACCESS;
    m_prevLinearDepthState[1] = RHI::ResourceState::UNORDERED_ACCESS;

    // 5-mip linear depth pyramid (reference XE_GTAO_DEPTH_MIP_LEVELS). Prefilter
    // dispatch writes all mips in one pass via u0..u4 bindings.
    if (m_linearDepth.IsValid()) m_gfx->DestroyTexture(m_linearDepth);
    {
        RHI::TextureDesc td{};
        td.width      = m_vpW;
        td.height     = m_vpH;
        td.mip_levels = kDepthMipLevels;
        td.format     = RHI::Format::R32_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!m_gfx->CreateTexture(td, m_linearDepth))
            LOG_ERROR("XeGTAOPass: failed to create linear depth pyramid");
    }

    m_aoRawState       = RHI::ResourceState::UNORDERED_ACCESS;
    m_aoFinalState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_edgesState       = RHI::ResourceState::UNORDERED_ACCESS;
    m_linearDepthState = RHI::ResourceState::UNORDERED_ACCESS;
    m_texDirty         = false;

    LOG_INFO("XeGTAOPass: created AO textures %ux%u + linear depth", m_vpW, m_vpH);
}

// ---------------------------------------------------------------------------
uint64_t XeGTAOPass::GetAOSrvHandle() const
{
    // Final AO now lives in the temporal-history buffer that Execute wrote
    // this frame (see m_historyFreshIdx assignment at the bottom of Execute).
    // Before any Execute has run the buffer isn't valid, so fall back to 0.
    if (!m_gfx) return 0;
    const RHI::Texture& tex = m_aoHistory[m_historyFreshIdx];
    if (!tex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(tex);
}

// ---------------------------------------------------------------------------
void XeGTAOPass::Execute(RHI::CommandList cl)
{
    if (!m_mainPSO.IsValid() || !m_denoisePSO.IsValid() ||
        !m_prefilterPSO.IsValid()) return;
    if (!m_depthSrvHandle || m_vpW == 0 || m_vpH == 0) return;

    if (m_texDirty) RebuildTextures();
    if (!m_aoRaw.IsValid() || !m_aoFinal.IsValid() ||
        !m_edges.IsValid() || !m_linearDepth.IsValid()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // ---- Extract projection-derived constants (shared by main + prefilter) --
    const float* pm = &m_projMatrix.m[0][0];
    float depthLinearizeMul = -pm[3 * 4 + 2];
    float depthLinearizeAdd =  pm[2 * 4 + 2];
    if (depthLinearizeMul * depthLinearizeAdd < 0.0f)
        depthLinearizeAdd = -depthLinearizeAdd;
    const float tanHalfFOVY = 1.0f / pm[1 * 4 + 1];
    const float tanHalfFOVX = 1.0f / pm[0 * 4 + 0];

    // ---- Fill ALL CBs up-front (different byte offsets in m_cb) ------------
    if (m_cbMapped)
    {
        uint8_t* base = static_cast<uint8_t*>(m_cbMapped);

        // Main GTAOConstants — offset 0.
        {
            GTAOConstants c{};
            c.ViewportSize[0]      = int32_t(m_vpW);
            c.ViewportSize[1]      = int32_t(m_vpH);
            c.ViewportPixelSize[0] = 1.0f / float(m_vpW);
            c.ViewportPixelSize[1] = 1.0f / float(m_vpH);
            c.DepthUnpackConsts[0] = depthLinearizeMul;
            c.DepthUnpackConsts[1] = depthLinearizeAdd;
            c.CameraTanHalfFOV[0]  = tanHalfFOVX;
            c.CameraTanHalfFOV[1]  = tanHalfFOVY;
            c.NDCToViewMul[0]      = tanHalfFOVX *  2.0f;
            c.NDCToViewMul[1]      = tanHalfFOVY * -2.0f;
            c.NDCToViewAdd[0]      = tanHalfFOVX * -1.0f;
            c.NDCToViewAdd[1]      = tanHalfFOVY *  1.0f;
            c.NDCToViewMul_x_PixelSize[0] = c.NDCToViewMul[0] * c.ViewportPixelSize[0];
            c.NDCToViewMul_x_PixelSize[1] = c.NDCToViewMul[1] * c.ViewportPixelSize[1];
            c.EffectRadius             = effectRadius;
            c.EffectFalloffRange       = effectFalloffRange;
            c.RadiusMultiplier         = radiusMultiplier;
            c.FinalValuePower          = finalValuePower;
            c.DenoiseBlurBeta          = denoiseBlurBeta;
            c.SampleDistributionPower  = sampleDistributionPower;
            c.ThinOccluderCompensation = thinOccluderCompensation;
            c.DepthMIPSamplingOffset   = depthMIPSamplingOffset;
            c.NoiseIndex               = int32_t(m_frameCounter % 64);
            c.SliceCount               = sliceCount;
            c.StepsPerSlice            = stepsPerSlice;
            // View matrix raw copy (row-major stored; HLSL reads column-major,
            // which yields the correct world→view rotation under mul(M, v)).
            std::memcpy(c.ViewMatrix, &m_viewMatrix.m[0][0], sizeof(c.ViewMatrix));
            std::memcpy(base + kCBOffsetMain, &c, sizeof(c));
        }

        // Temporal CB — accumulates AO across frames via velocity reprojection.
        // historyAlpha = 0 on the first valid frame (or after a resize) so
        // the shader falls through to "use current" instead of sampling an
        // uninitialised history buffer.
        {
            TemporalCB tc{};
            tc.viewportWidth  = m_vpW;
            tc.viewportHeight = m_vpH;
            tc.historyAlpha   = m_historyValid ? temporalHistoryAlpha : 0.0f;
            tc.rejectionDiff  = temporalRejectionDiff;
            std::memcpy(base + kCBOffsetTemporal, &tc, sizeof(tc));
        }

        // Denoise CB — offset 512. Single-pass final denoise matches the
        // reference's default `DenoisePasses = 1` setting (one call to
        // CSDenoiseLastPass = finalApply = true). To extend to multi-pass,
        // add an intermediate-AO texture, issue N-1 dispatches with
        // finalApply=0 ping-ponging between aoRaw and scratch, then one
        // finalApply=1 dispatch into aoFinal.
        {
            DenoiseCB cb{};
            cb.viewportWidth     = m_vpW;
            cb.viewportHeight    = m_vpH;
            cb.depthLinearizeMul = depthLinearizeMul;
            cb.depthLinearizeAdd = depthLinearizeAdd;
            cb.denoiseBlurBeta   = denoiseBlurBeta;
            cb.finalApply        = 1u;
            std::memcpy(base + kCBOffsetDenoise, &cb, sizeof(cb));
        }

        // Prefilter CB — single linearize dispatch (no pyramid).
        {
            PrefilterCB pc{};
            pc.srcSize[0]           = m_vpW;
            pc.srcSize[1]           = m_vpH;
            pc.dstSize[0]           = m_vpW;
            pc.dstSize[1]           = m_vpH;
            pc.depthUnpackConsts[0] = depthLinearizeMul;
            pc.depthUnpackConsts[1] = depthLinearizeAdd;
            pc.effectRadius         = effectRadius;
            pc.effectFalloffRange   = effectFalloffRange;
            pc.radiusMultiplier     = radiusMultiplier;
            pc.srcMip               = 0;
            std::memcpy(base + kCBOffsetPrefilter, &pc, sizeof(pc));
        }
    }

    // ---- Pass A: 5-mip depth prefilter (single dispatch) -------------------
    // Writes all 5 linear-depth mips via u0..u4 bindings, matching Intel's
    // XeGTAO_PrefilterDepths16x16. Thread group = 8×8 = one 2×2 quad per
    // thread in mip 0 → dispatch covers (width+15)/16 × (height+15)/16 tiles.
    {
        if (m_linearDepthState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_linearDepth, m_linearDepthState, RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_linearDepthState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_prefilterPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cb, kCBOffsetPrefilter, cl);
        gfx.SetComputeDescriptorTable(kSRV0, m_depthSrvHandle, cl);
        // u0..u4 — one UAV per mip level.
        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_linearDepth, 0), cl);
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureMipUAVGpuHandle(m_linearDepth, 1), cl);
        gfx.SetComputeDescriptorTable(kUAV2,
            gfx.GetTextureMipUAVGpuHandle(m_linearDepth, 2), cl);
        gfx.SetComputeDescriptorTable(kUAV3,
            gfx.GetTextureMipUAVGpuHandle(m_linearDepth, 3), cl);
        gfx.SetComputeDescriptorTable(kUAV4,
            gfx.GetTextureMipUAVGpuHandle(m_linearDepth, 4), cl);

        // Each thread covers a 2×2 mip-0 quad, threadgroup is 8×8 ⇒ 16×16 px.
        gfx.DispatchCompute((m_vpW + 15) / 16, (m_vpH + 15) / 16, 1, cl);

        // Whole pyramid → SRV_COMPUTE for the main pass's SampleLevel reads.
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_linearDepth, RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_linearDepthState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }

    // ---- Pass B: GTAO main -------------------------------------------------
    {
        if (m_aoRawState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_aoRaw, m_aoRawState, RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_aoRawState = RHI::ResourceState::UNORDERED_ACCESS;
        }
        if (m_edgesState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_edges, m_edgesState, RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_edgesState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_mainPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cb, kCBOffsetMain, cl);
        gfx.SetComputeDescriptorTable(kSRV0,
            gfx.GetTextureSRVGpuHandle(m_linearDepth), cl);
        if (m_normalSrvHandle)
            gfx.SetComputeDescriptorTable(kSRV1, m_normalSrvHandle, cl);
        // t2 space2 — Hilbert LUT used by SpatioTemporalNoise.
        if (m_hilbertLUTSrv)
            gfx.SetComputeDescriptorTable(kSRV2, m_hilbertLUTSrv, cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_aoRaw), cl);
        gfx.SetComputeDescriptorTable(kUAV1, gfx.GetTextureUAVGpuHandle(m_edges), cl);
        gfx.DispatchCompute((m_vpW + 7) / 8, (m_vpH + 7) / 8, 1, cl);
    }

    // Transition raw AO + edges to SRV_COMPUTE for denoise + temporal reads.
    // NOTE the pass order is now: Main → Spatial Denoise → Temporal (final
    // output). Doing denoise first gives temporal a low-variance input, which
    // is critical: variance clip against *raw* 1-spp blue-noise AO has a σ
    // wide enough to swallow stale history values as "plausible", producing
    // the crevice ghost trails. Against the denoised signal, σ collapses and
    // the clip becomes a real disocclusion test.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_aoRaw, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_aoRawState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_edges, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_edgesState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // ---- Pass C: Spatial denoise (was Pass D) ------------------------------
    // Reads raw AO + edges, writes m_aoFinal as the *intermediate* denoised
    // signal that feeds temporal below. m_aoFinal is no longer "the final AO"
    // semantically — the final output is the temporal-history buffer we
    // return from GetAOSrvHandle().
    {
        if (m_aoFinalState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_aoFinal, m_aoFinalState, RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_aoFinalState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_denoisePSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cb, kCBOffsetDenoise, cl);
        gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_aoRaw), cl);
        gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_edges), cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_aoFinal), cl);
        // Each thread denoises 2×1 pixels → halve the X dispatch count.
        const uint32_t halfW = (m_vpW + 1u) / 2u;
        gfx.DispatchCompute((halfW + 7u) / 8u, (m_vpH + 7u) / 8u, 1, cl);
    }

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_aoFinal, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_aoFinalState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // ---- Pass D: Temporal accumulation (was Pass C) ------------------------
    // Reads the *denoised* AO (not raw) + velocity + edges + prev history.
    // Writes this frame's history, which IS the final output returned to
    // LightingPass via GetAOSrvHandle. With the denoised input the variance
    // clip can run tight (gamma=1.0, σ floor 0.02) and rejectionDiff 0.05
    // works as a real disocclusion signal instead of a blue-noise safety
    // margin. Edges feed an α modulation so disocclusion-prone pixels trust
    // new data faster.
    //
    // Velocity state: no explicit barrier here. The render graph transitions
    // velocity to SHADER_RESOURCE at the end of Graph::Execute (same state
    // TAAPass reads it in above), and XeGTAO runs on the same command list
    // immediately after TAA. If TAA is disabled, velocity is still in
    // SHADER_RESOURCE because that's the graph's post-Execute state.
    const int writeIdx = int(m_historyWriteIdx);
    const int readIdx  = 1 - writeIdx;
    {
        // Write target → UAV.
        if (m_aoHistoryState[writeIdx] != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_aoHistory[writeIdx],
                m_aoHistoryState[writeIdx],
                RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_aoHistoryState[writeIdx] = RHI::ResourceState::UNORDERED_ACCESS;
        }
        // Read source (history) → SRV. On the very first frame it's still in
        // UAV creation state; the shader ignores it when historyAlpha == 0 but
        // the descriptor still must be bound from a legal SRV state.
        if (m_aoHistoryState[readIdx] != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_aoHistory[readIdx],
                m_aoHistoryState[readIdx],
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
            m_aoHistoryState[readIdx] = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        }

        // Prev-depth write target (next frame's read source) → UAV.
        if (m_prevLinearDepthState[writeIdx] != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_prevLinearDepth[writeIdx],
                m_prevLinearDepthState[writeIdx],
                RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_prevLinearDepthState[writeIdx] = RHI::ResourceState::UNORDERED_ACCESS;
        }
        // Prev-depth read source (from last frame's write) → SRV. On the
        // very first frame it's in UAV creation state; shader gates the
        // depth rejection on historyValid anyway, so sampling uninitialised
        // data there is harmless (it's multiplied by α=0).
        if (m_prevLinearDepthState[readIdx] != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_prevLinearDepth[readIdx],
                m_prevLinearDepthState[readIdx],
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
            m_prevLinearDepthState[readIdx] = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        }
        // m_linearDepth is already SHADER_RESOURCE_COMPUTE from Pass B main's
        // post-barrier (see above); no transition needed.

        gfx.BindComputePipelineState(m_temporalPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cb, kCBOffsetTemporal, cl);
        // t0 space2 = denoised AO (was raw AO before the reorder).
        gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_aoFinal), cl);
        // t1 space2 = packed edge mask — drives α modulation in the shader.
        gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_edges), cl);
        gfx.SetComputeDescriptorTable(kSRV2,
            gfx.GetTextureSRVGpuHandle(m_aoHistory[readIdx]), cl);
        // t3 space2 (root slot 7) = prev-frame linear depth (read source).
        gfx.SetComputeDescriptorTable(/*t3 slot*/ 7,
            gfx.GetTextureSRVGpuHandle(m_prevLinearDepth[readIdx]), cl);
        // Velocity → root slot 8 = t4 space2. Engine-wide convention (TAA
        // uses the same slot); keeps bindings legible when multiple compute
        // passes read GBuffer velocity.
        if (m_velocitySrvHandle)
            gfx.SetComputeDescriptorTable(/*velocity slot*/ 8, m_velocitySrvHandle, cl);
        // t5 space2 (root slot 6) = current-frame linearised depth pyramid.
        // Shader reads mip 0 via Load(int3(px, 0)) — the slot is morph-weights
        // in SceneVoxelize.cs but legal to repurpose (the runtime only
        // enforces matching descriptor type / visibility, not semantic use).
        gfx.SetComputeDescriptorTable(/*t5 slot*/ 6,
            gfx.GetTextureSRVGpuHandle(m_linearDepth), cl);

        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureUAVGpuHandle(m_aoHistory[writeIdx]), cl);
        // u1 space2 = destination for this-frame depth copy → becomes next
        // frame's prev-depth read source.
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureUAVGpuHandle(m_prevLinearDepth[writeIdx]), cl);
        gfx.DispatchCompute((m_vpW + 7) / 8, (m_vpH + 7) / 8, 1, cl);
    }
    // Transition write buffer → SRV so LightingPass (next frame) can read it.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_aoHistory[writeIdx],
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_aoHistoryState[writeIdx] = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // Prev-depth write buffer → SRV for next frame's temporal read.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_prevLinearDepth[writeIdx],
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_prevLinearDepthState[writeIdx] = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // Record which history buffer holds the just-written "final" AO — it's
    // what GetAOSrvHandle returns until the next Execute. Then swap write/
    // read so next frame writes the opposite slot.
    m_historyFreshIdx = uint32_t(writeIdx);
    m_historyWriteIdx = uint32_t(readIdx);
    m_historyValid    = true;
}
