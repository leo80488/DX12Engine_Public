#include "Graphics/SSR/SSRSubsystem.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"   // CreateTexturePlaced + CreateHeap (alias path)
#include "System/Log.h"
#include "Graphics/SSR/SSRTracePass.h"
#include "Graphics/SSR/SSRResolvePass.h"
#include "Graphics/SSR/SSRTemporalPass.h"
#include "Graphics/SSR/SSRUpsamplePass.h"
#include "Graphics/SSR/SSRCompositePass.h"
#include "Graphics/SSR/SSRDepthHierarchyPass.h"
#include "Graphics/SSR/SceneColorPyramidPass.h"

#include <cstring>

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
SSRSubsystem::SSRSubsystem()  = default;
SSRSubsystem::~SSRSubsystem() = default;

void SSRSubsystem::Init(IGraphicsDevice& gfx)
{
    // Construction order = pipeline order — the sub-passes themselves don't
    // depend on each other at Init time, but this keeps the destructor + log
    // sequence readable.
    m_trace = std::make_unique<SSRPass>();
    m_trace->Init(gfx);

    m_resolve = std::make_unique<SSRResolvePass>();
    m_resolve->Init(gfx);

    m_temporal = std::make_unique<SSRTemporalPass>();
    m_temporal->Init(gfx);

    m_upsample = std::make_unique<SSRUpsamplePass>();
    m_upsample->Init(gfx);

    m_composite = std::make_unique<SSRCompositePass>();
    m_composite->Init(gfx);

    m_depthHier = std::make_unique<SSRDepthHierarchyPass>();
    m_depthHier->Init(gfx);

    // Karis-firefly HDR mip chain for trace cone-footprint sampling.
    m_sceneColorPyr = std::make_unique<SceneColorPyramidPass>();
    m_sceneColorPyr->Init(gfx);
}

void SSRSubsystem::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_trace)         m_trace->ReloadShaders(gfx);
    if (m_resolve)       m_resolve->ReloadShaders(gfx);
    if (m_temporal)      m_temporal->ReloadShaders(gfx);
    if (m_upsample)      m_upsample->ReloadShaders(gfx);
    if (m_composite)     m_composite->ReloadShaders(gfx);
    if (m_depthHier)     m_depthHier->ReloadShaders(gfx);
    if (m_sceneColorPyr) m_sceneColorPyr->ReloadShaders(gfx);
}

uint64_t SSRSubsystem::OnResize(uint32_t renderW, uint32_t renderH)
{
    if (!m_trace) return 0;

    m_trace->EnsureTexture(renderW, renderH);
    // When scratch aliasing is enabled, the resolve snapshot+color must be the
    // PLACED variant created in Render() (with the alias heap) — skip the
    // committed creation here so it isn't created first and then early-out'd.
    if (m_resolve && !kSSREnableScratchAliasing) m_resolve->EnsureTextures(renderW, renderH);
    if (m_temporal) m_temporal->EnsureTextures(renderW, renderH);
    if (m_upsample) m_upsample->EnsureTexture(renderW, renderH);

    // Preferred binding is the upsample output (LightingPass dampens IBL by
    // 1 - ssrConf with 1-frame latency). Fall back to raw trace if upsample
    // isn't created yet — matches the prior Renderer-level guard.
    if (m_upsample && m_upsample->GetColorSrv())
        return m_upsample->GetColorSrv();
    return m_trace->GetResultSrv();
}

uint64_t SSRSubsystem::GetFinalColorSrv() const
{
    if (m_upsample) return m_upsample->GetColorSrv();
    if (m_trace)    return m_trace->GetResultSrv();
    return 0;
}

uint64_t SSRSubsystem::GetTraceResultSrv() const
{
    return m_trace ? m_trace->GetResultSrv() : 0;
}

// ---------------------------------------------------------------------------
// Transient scratch-alias heap (gated by kSSREnableScratchAliasing)
// ---------------------------------------------------------------------------
bool SSRSubsystem::EnsureAliasHeap(GraphicsDX12& gfx, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return false;
    if (m_aliasHeap && w == m_aliasHeapW && h == m_aliasHeapH) return true;

    // Size for one full-res RGBA16F SR|UAV 2D texture — both the snapshot
    // (SR-only) and the resolve color (SR|UAV) fit; they alias at offset 0.
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const D3D12_RESOURCE_ALLOCATION_INFO ai =
        gfx.GetDevice()->GetResourceAllocationInfo(0, 1, &rd);
    if (ai.SizeInBytes == 0 || ai.SizeInBytes == UINT64_MAX) return false;

    // Retire the old heap: its placed resources are about to be DestroyTexture'd
    // (deferred FrameCount frames). Free the heap one cycle later so it always
    // outlives the placed resources referencing it (the key TDR-avoidance rule).
    if (m_aliasHeap)
        m_retiredHeaps.emplace_back(std::move(m_aliasHeap), GraphicsDX12::FrameCount + 1u);

    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes        = ai.SizeInBytes;
    hd.Properties.Type    = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // 64KB
    hd.Flags              = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES
                          | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
    if (FAILED(gfx.GetDevice()->CreateHeap(&hd, IID_PPV_ARGS(&m_aliasHeap))))
    {
        LOG_ERROR("SSRSubsystem: alias-heap CreateHeap failed (%llu bytes)",
                  (unsigned long long)ai.SizeInBytes);
        m_aliasHeap.Reset();
        return false;
    }
    m_aliasHeapSize = ai.SizeInBytes;
    m_aliasHeapW = w; m_aliasHeapH = h;
    return true;
}

void SSRSubsystem::TickAliasHeap()
{
    for (auto it = m_retiredHeaps.begin(); it != m_retiredHeaps.end(); )
    {
        if (--it->second == 0) it = m_retiredHeaps.erase(it);
        else                   ++it;
    }
}

// ---------------------------------------------------------------------------
// Render — Phase 4.6 (trace chain) + Phase 4.7 (composite)
//
// Lifted verbatim from Renderer.cpp's pre-extraction layout. Every barrier
// and CL dependency stays exactly as it was so this phase is a behaviour-
// preserving refactor — algorithm changes land in Phase 2 onward.
// ---------------------------------------------------------------------------
void SSRSubsystem::Render(IGraphicsDevice& gfx,
                          const FrameContext& ctx,
                          RHI::CommandList colorLastCL)
{
    if (!m_trace || !m_resolve || !m_depthHier) return;
    if (!ctx.graph) return;

    auto& graph = *ctx.graph;

    const RHI::Texture* depthTex    = graph.GetPhysicalTexture(ctx.depthHandle);
    const RHI::Texture* normalTex   = graph.GetPhysicalTexture(ctx.normalHandle);
    const RHI::Texture* surfaceTex  = graph.GetPhysicalTexture(ctx.surfaceHandle);
    const RHI::Texture* velocityTex = graph.GetPhysicalTexture(ctx.velocityHandle);
    if (!depthTex || !normalTex || !surfaceTex) return;

    const uint32_t rw = gfx.GetRenderWidth();
    const uint32_t rh = gfx.GetRenderHeight();

    m_trace->EnsureTexture(rw, rh);
    m_depthHier->EnsureTexture(rw, rh);
    if (kSSREnableScratchAliasing)
    {
        // snapshot + color share one heap (lifetime-disjoint, same size).
        auto& dx12 = static_cast<GraphicsDX12&>(gfx);
        TickAliasHeap();
        if (EnsureAliasHeap(dx12, rw, rh))
            m_resolve->EnsureTextures(rw, rh, m_aliasHeap.Get(), /*snapshot*/0, /*color*/0);
        else
            m_resolve->EnsureTextures(rw, rh); // heap alloc failed → committed fallback
    }
    else
    {
        m_resolve->EnsureTextures(rw, rh);
    }

    // ----- Per-frame camera state (jittered) --------------------------------
    using namespace DirectX;
    SSRPass::Camera traceCam{};
    traceCam.viewProj = ctx.viewProj;
    {
        XMMATRIX vp = XMLoadFloat4x4(&traceCam.viewProj);
        XMStoreFloat4x4(&traceCam.invViewProj, XMMatrixInverse(nullptr, vp));
    }
    traceCam.cameraPos = ctx.cameraPos;
    traceCam.nearZ     = ctx.nearZ;
    traceCam.farZ      = ctx.farZ;
    // One frame index shared by trace / resolve / temporal so their sub-pixel
    // jitter tables index the same slot. Post-increment AFTER the three Set*
    // calls so this-frame's value is used everywhere.
    const uint32_t frameIdx = m_frameIndex;
    m_trace->SetCamera(traceCam);
    m_trace->SetFrameIndex(frameIdx);
    {
        // Hi-Z mip count = 1 + floor(log2(max(w,h))).
        uint32_t mip = 1, dim = (rw > rh) ? rw : rh;
        while (dim > 1) { dim >>= 1; mip++; }
        m_trace->SetHiZMipCount(mip);
    }

    SSRResolvePass::Camera rcam{};
    rcam.invViewProj = traceCam.invViewProj;
    rcam.cameraPos   = traceCam.cameraPos;
    rcam.nearZ       = traceCam.nearZ;
    rcam.farZ        = traceCam.farZ;
    m_resolve->SetCamera(rcam);
    m_resolve->SetFrameIndex(frameIdx);

    SSRTemporalPass::Camera tcam{};
    tcam.invViewProj  = traceCam.invViewProj;
    tcam.prevViewProj = ctx.prevViewProjJittered;
    tcam.nearZ        = traceCam.nearZ;
    tcam.farZ         = traceCam.farZ;
    if (m_temporal)
    {
        m_temporal->SetCamera(tcam);
        // Same frame index as trace/resolve so all three see the same
        // sub-pixel jitter and read aligned full-res GBuffer texels.
        m_temporal->SetFrameIndex(frameIdx);
    }
    // Advance for next frame now that all three sub-passes have been seeded.
    ++m_frameIndex;

    SSRUpsamplePass::Camera ucam{};
    ucam.invViewProj = traceCam.invViewProj;
    ucam.nearZ       = traceCam.nearZ;
    ucam.farZ        = traceCam.farZ;
    if (m_upsample) m_upsample->SetCamera(ucam);

    if (m_sceneColorPyr) m_sceneColorPyr->EnsureTexture(rw, rh);
    if (m_temporal)      m_temporal->EnsureTextures(rw, rh);
    if (m_upsample)      m_upsample->EnsureTexture(rw, rh);

    // ====================================================================
    // Phase 4.6 — trace chain
    // ====================================================================
    RHI::CommandList ssrCL = gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
    ssrCL.gfx = &gfx;
    if (colorLastCL.IsValid())
        gfx.AddCommandListDependency(ssrCL, colorLastCL);

    const RHI::ResourceState depthState0    = graph.GetTextureState(ctx.depthHandle);
    const RHI::ResourceState normalState0   = graph.GetTextureState(ctx.normalHandle);
    const RHI::ResourceState surfaceState0  = graph.GetTextureState(ctx.surfaceHandle);
    const RHI::ResourceState velocityState0 = velocityTex
        ? graph.GetTextureState(ctx.velocityHandle)
        : RHI::ResourceState::SHADER_RESOURCE;

    auto toSR = [&](const RHI::Texture* t, RHI::ResourceState from) {
        if (!t) return;
        if (from != RHI::ResourceState::SHADER_RESOURCE)
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                t, from, RHI::ResourceState::SHADER_RESOURCE), ssrCL);
    };
    toSR(depthTex,    depthState0);
    toSR(normalTex,   normalState0);
    toSR(surfaceTex,  surfaceState0);
    toSR(velocityTex, velocityState0);

    // Per-sub-pass GPU timestamps — SSRSubsystem is logically multi-pass,
    // matches the SkyIBL / VolFog convention so each sub-step appears in the
    // profiler panel rather than collapsing to a single "SSR" line.
    auto pBegin = [&](const char* name) -> uint32_t {
        return gfx.BeginGPUTimestamp(ssrCL, name);
    };
    auto pEnd = [&](uint32_t r) {
        gfx.EndGPUTimestamp(ssrCL, r);
    };

    // 1. Depth pyramid (internal chain leaves the texture in UAV).
    {
        uint32_t r = pBegin("SSR.DepthHier");
        m_depthHier->Execute(ssrCL, gfx.GetTextureSRVGpuHandle(*depthTex));
        pEnd(r);
    }
    const RHI::Texture* hierTex = m_depthHier->GetTexture();
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        hierTex, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), ssrCL);

    // 2. HDR snapshot + scene-color pyramid (trace consumes mip-chain).
    {
        uint32_t r = pBegin("SSR.SnapshotCopy");
        const RHI::Texture* snapTex = m_resolve->GetSnapshotTexture();
        gfx.SetHdrTextureState(RHI::ResourceState::COPY_SRC, ssrCL);
        if (kSSREnableScratchAliasing && m_aliasHeap)
        {
            // Hand the shared heap bytes to snapshot (color is now inactive).
            // Do NOT reset the engine-tracked state: the debug layer tracks
            // resource state ACROSS aliasing barriers, so PreSnapshotCopy's
            // normal transition (from snapshot's real prior state) is what the
            // layer expects. Content is re-initialized by the full CopyHdrSceneTo
            // below (the aliasing init contract).
            gfx.PushBarrier(RHI::GPUBarrier::Aliasing(
                m_resolve->GetColorTexture(), m_resolve->GetSnapshotTexture()), ssrCL);
        }
        m_resolve->PreSnapshotCopy(ssrCL);
        gfx.CopyHdrSceneTo(*snapTex, ssrCL);
        m_resolve->PostSnapshotCopy(ssrCL);
        pEnd(r);
    }

    uint64_t pyramidSrv = m_resolve->GetSnapshotSrv();  // mip-0-only fallback
    if (m_sceneColorPyr)
    {
        uint32_t r = pBegin("SSR.SceneColorPyramid");
        m_sceneColorPyr->Execute(ssrCL, m_resolve->GetSnapshotTexture());
        pEnd(r);
        const RHI::Texture* pyrTex = m_sceneColorPyr->GetTexture();
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            pyrTex, RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE), ssrCL);
        pyramidSrv = m_sceneColorPyr->GetSrvHandle();
    }

    // 3. Trace — writes hit color + rayDirPDF + rayLength.
    {
        uint32_t r = pBegin("SSR.Trace");
        m_trace->Execute(ssrCL,
            gfx.GetTextureSRVGpuHandle(*normalTex),
            gfx.GetTextureSRVGpuHandle(*surfaceTex),
            gfx.GetTextureSRVGpuHandle(*depthTex),
            m_depthHier->GetSrvHandle(),
            pyramidSrv,
            velocityTex ? gfx.GetTextureSRVGpuHandle(*velocityTex) : 0);
        pEnd(r);
    }

    // All three trace outputs UAV → SR for the resolve dispatch.
    auto uavToSR = [&](const RHI::Texture* t) {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            t, RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE), ssrCL);
    };
    uavToSR(m_trace->GetResultTexture());
    uavToSR(m_trace->GetRayDirPDFTexture());
    uavToSR(m_trace->GetRayLengthTexture());
    m_trace->SetAllOutputsState(RHI::ResourceState::SHADER_RESOURCE);

    // Phase 7: all SSR sub-passes run at FULL render resolution. The
    // half-res variant from Phase 3 caused visible noise without TAA + sub-
    // pixel reflection misalignment; re-introduce only with a denoiser
    // that's robust without TAA jitter.

    // 4. Resolve (spatial BRDF reweight + variance).
    {
        uint32_t r = pBegin("SSR.Resolve");
        if (kSSREnableScratchAliasing && m_aliasHeap)
        {
            // Hand the shared heap bytes back to color (snapshot finished being
            // read by the pyramid/trace above). No tracked-state reset (see the
            // snapshot barrier above); resolve's full-coverage UAV write
            // re-initializes the aliased memory.
            gfx.PushBarrier(RHI::GPUBarrier::Aliasing(
                m_resolve->GetSnapshotTexture(), m_resolve->GetColorTexture()), ssrCL);
        }
        m_resolve->Execute(ssrCL, rw, rh,
            gfx.GetTextureSRVGpuHandle(*normalTex),
            gfx.GetTextureSRVGpuHandle(*surfaceTex),
            gfx.GetTextureSRVGpuHandle(*depthTex),
            m_trace->GetResultSrv(),
            m_trace->GetRayDirPDFSrv(),
            m_trace->GetRayLengthSrv());
        pEnd(r);
    }

    // 5. Temporal (dual-reprojection history blend).
    if (m_temporal)
    {
        uint32_t r = pBegin("SSR.Temporal");
        m_temporal->Execute(ssrCL, rw, rh,
            m_resolve->GetColorSrv(),
            m_resolve->GetVarianceSrv(),
            m_resolve->GetReprojDepthSrv(),
            velocityTex ? gfx.GetTextureSRVGpuHandle(*velocityTex) : 0,
            gfx.GetTextureSRVGpuHandle(*depthTex));
        pEnd(r);
    }

    // 6. Upsample (variance-driven bilateral).
    if (m_upsample && m_temporal)
    {
        uint32_t r = pBegin("SSR.Upsample");
        m_upsample->Execute(ssrCL, rw, rh,
            m_temporal->GetColorSrv(),
            m_temporal->GetVarianceSrv(),
            gfx.GetTextureSRVGpuHandle(*depthTex),
            gfx.GetTextureSRVGpuHandle(*normalTex),
            gfx.GetTextureSRVGpuHandle(*surfaceTex));
        pEnd(r);
    }

    // Return scene-color pyramid to UAV for next frame's mip 0 write.
    if (m_sceneColorPyr)
    {
        const RHI::Texture* pyrTex = m_sceneColorPyr->GetTexture();
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            pyrTex, RHI::ResourceState::SHADER_RESOURCE,
            RHI::ResourceState::UNORDERED_ACCESS), ssrCL);
    }

    // Return the depth pyramid to UAV for next frame's mip 0 dispatch.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        hierTex, RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), ssrCL);

    // HDR → RT so Phase 4.7 composite can flip it to UAV cleanly.
    gfx.SetHdrTextureState(RHI::ResourceState::RENDERTARGET, ssrCL);

    // Restore graph-tracked states so downstream passes see what they expect.
    auto fromSR = [&](const RHI::Texture* t, RHI::ResourceState to) {
        if (!t) return;
        if (to != RHI::ResourceState::SHADER_RESOURCE)
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                t, RHI::ResourceState::SHADER_RESOURCE, to), ssrCL);
    };
    fromSR(depthTex,    depthState0);
    fromSR(normalTex,   normalState0);
    fromSR(surfaceTex,  surfaceState0);
    fromSR(velocityTex, velocityState0);

    // ====================================================================
    // Phase 4.7 — composite
    // Resolved SSR + Fresnel × envBRDF → HDR additive; pairs with Lighting's
    // own (1 - ssrConf) IBL dampening so the combined result is energy-correct.
    // ====================================================================
    if (!m_upsample || !m_composite) return;

    const RHI::Texture* albedoTex = graph.GetPhysicalTexture(ctx.albedoHandle);
    const RHI::Texture* ssrTex    = m_upsample->GetColorTexture();
    const uint64_t      hdrUav    = gfx.GetHdrSceneUavGpuHandle();
    if (!albedoTex || !ssrTex || !hdrUav) return;

    SSRCompositePass::Camera ccam{};
    ccam.cameraPos = ctx.cameraPos;
    {
        XMMATRIX vp = XMLoadFloat4x4(&ctx.viewProj);
        XMStoreFloat4x4(&ccam.invViewProj, XMMatrixInverse(nullptr, vp));
    }
    m_composite->SetCamera(ccam);

    RHI::CommandList compCL = gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
    compCL.gfx = &gfx;
    if (colorLastCL.IsValid())
        gfx.AddCommandListDependency(compCL, colorLastCL);

    const RHI::ResourceState albedoState0   = graph.GetTextureState(ctx.albedoHandle);
    const RHI::ResourceState normalState0c  = graph.GetTextureState(ctx.normalHandle);
    const RHI::ResourceState surfaceState0c = graph.GetTextureState(ctx.surfaceHandle);
    const RHI::ResourceState depthState0c   = graph.GetTextureState(ctx.depthHandle);
    auto toSR_c = [&](const RHI::Texture* t, RHI::ResourceState from) {
        if (from != RHI::ResourceState::SHADER_RESOURCE)
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                t, from, RHI::ResourceState::SHADER_RESOURCE), compCL);
    };
    toSR_c(albedoTex,  albedoState0);
    toSR_c(normalTex,  normalState0c);
    toSR_c(surfaceTex, surfaceState0c);
    toSR_c(depthTex,   depthState0c);

    gfx.SetHdrTextureState(RHI::ResourceState::UNORDERED_ACCESS, compCL);

    // Debug SRVs already left in SHADER_RESOURCE by the trace block above.
    const uint64_t rawTraceSrv = m_trace->GetResultSrv();
    const uint64_t rayDirSrv   = m_trace->GetRayDirPDFSrv();

    // Keep composite's roughness mask in sync with trace's runtime cutoff.
    m_composite->SetRoughnessCutoff(m_trace->GetRoughnessCutoff());

    {
        uint32_t r = gfx.BeginGPUTimestamp(compCL, "SSR.Composite");
        m_composite->Execute(compCL, rw, rh,
            gfx.GetTextureSRVGpuHandle(*albedoTex),
            gfx.GetTextureSRVGpuHandle(*normalTex),
            gfx.GetTextureSRVGpuHandle(*surfaceTex),
            gfx.GetTextureSRVGpuHandle(*depthTex),
            gfx.GetTextureSRVGpuHandle(*ssrTex),
            hdrUav,
            ctx.brdfLutSrv,
            rawTraceSrv, rayDirSrv, /*rayLen*/0, /*variance*/0);
        gfx.EndGPUTimestamp(compCL, r);
    }

    gfx.SetHdrTextureState(RHI::ResourceState::SHADER_RESOURCE, compCL);

    auto fromSR_c = [&](const RHI::Texture* t, RHI::ResourceState to) {
        if (to != RHI::ResourceState::SHADER_RESOURCE)
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                t, RHI::ResourceState::SHADER_RESOURCE, to), compCL);
    };
    fromSR_c(albedoTex,  albedoState0);
    fromSR_c(normalTex,  normalState0c);
    fromSR_c(surfaceTex, surfaceState0c);
    fromSR_c(depthTex,   depthState0c);
}
