#pragma once

// SSRSubsystem — single owning class for the entire Hi-Z SSR pipeline.
//
// Before this existed, Renderer.cpp held seven unique_ptrs (trace / resolve /
// temporal / upsample / composite / depth-hier / scene-color-pyramid) and
// hand-orchestrated ~270 lines of barrier + command-list dance every frame.
// This class encapsulates all of that:
//
//   Init()       — create all sub-passes
//   OnResize()   — propagate render-target dimensions
//   Render()     — submit the trace chain (Phase 4.6) + composite (Phase 4.7),
//                  chaining off the caller's last graph command list.
//
// The per-pass classes themselves (SSRPass, SSRResolvePass, …) still live in
// include/RenderGraph/RenderPass/SSRPass.h — they are forward-declared here so
// the editor SSR-debug window can keep poking at their tunables via the
// Renderer-level getters (which now forward through this subsystem).

#include "Graphics/GraphicsStruct.h"
#include "RenderGraph/RenderGraph.h"

#include <memory>
#include <cstdint>
#include <vector>
#include <utility>
#include <wrl.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;
class GraphicsDX12;
class SSRPass;
class SSRResolvePass;
class SSRTemporalPass;
class SSRUpsamplePass;
class SSRCompositePass;
class SSRDepthHierarchyPass;
class SceneColorPyramidPass;

class SSRSubsystem
{
public:
    SSRSubsystem();
    ~SSRSubsystem();

    SSRSubsystem(const SSRSubsystem&)            = delete;
    SSRSubsystem& operator=(const SSRSubsystem&) = delete;

    // Create + Init all sub-passes. Call once during Renderer::Compile().
    void Init(IGraphicsDevice& gfx);

    // Hot-reload entry point: re-fetches every SSR compute shader from disk
    // and rebuilds the underlying PSOs. Wipes temporal history so the first
    // post-reload frame doesn't blend stale accumulator state.
    void ReloadShaders(IGraphicsDevice& gfx);

    // (Re)allocate every internal texture for the new render resolution. Safe
    // to call every frame; the sub-passes early-out if dimensions are
    // unchanged. Returns the upsample SRV so LightingPass can rebind for
    // (1 - ssrConf) IBL dampening on the next frame.
    uint64_t OnResize(uint32_t renderW, uint32_t renderH);

    // Per-frame inputs gathered by Renderer. Plain POD — no resource
    // ownership transfer.
    struct FrameContext
    {
        RG::RenderGraph*       graph         = nullptr;

        // GBuffer attachments (RG-managed). State is queried + restored via
        // graph->GetTextureState / SetTextureState so downstream passes still
        // see the state they expect.
        RG::RGTextureHandle    albedoHandle{};
        RG::RGTextureHandle    normalHandle{};
        RG::RGTextureHandle    surfaceHandle{};
        RG::RGTextureHandle    depthHandle{};
        RG::RGTextureHandle    velocityHandle{};
        // Optional grass-free depth snapshot (copied pre-GrassPass). When
        // valid, the Hi-Z march pyramid is built from THIS depth so thin
        // grass blades don't block reflection rays; per-pixel reads (trace
        // origins, finish refinement, resolve/temporal/upsample bilateral)
        // keep using depthHandle. Invalid → pyramid falls back to depthHandle.
        RG::RGTextureHandle    traceDepthHandle{};

        // Jittered camera state. invViewProj is derived internally.
        DirectX::XMFLOAT4X4    viewProj{};
        DirectX::XMFLOAT4X4    prevViewProjJittered{};
        DirectX::XMFLOAT3      cameraPos{};
        float                  nearZ         = 0.0f;
        float                  farZ          = 0.0f;

        // BRDF LUT for composite Fresnel × envBRDF. 0 disables composite.
        uint64_t               brdfLutSrv    = 0;
    };

    // Submit Phase 4.6 (depth pyramid → snapshot → scene color pyramid →
    // trace → resolve → temporal → upsample) and Phase 4.7 (composite).
    // Both internal command lists depend on @p colorLastCL.
    void Render(IGraphicsDevice& gfx,
                const FrameContext& ctx,
                RHI::CommandList colorLastCL);

    // ===== Pass accessors (SSRDebugWindow + Renderer forwarders) =====
    SSRPass*               GetTrace()        { return m_trace.get(); }
    SSRResolvePass*        GetResolve()      { return m_resolve.get(); }
    SSRTemporalPass*       GetTemporal()     { return m_temporal.get(); }
    SSRUpsamplePass*       GetUpsample()     { return m_upsample.get(); }
    SSRCompositePass*      GetComposite()    { return m_composite.get(); }
    SSRDepthHierarchyPass* GetDepthHier()    { return m_depthHier.get(); }
    SceneColorPyramidPass* GetSceneColorPyr(){ return m_sceneColorPyr.get(); }

    // Convenience SRVs. Final = upsample output (LightingPass binds this).
    uint64_t GetFinalColorSrv() const;
    uint64_t GetTraceResultSrv() const;

private:
    std::unique_ptr<SSRPass>               m_trace;
    std::unique_ptr<SSRResolvePass>        m_resolve;
    std::unique_ptr<SSRTemporalPass>       m_temporal;
    std::unique_ptr<SSRUpsamplePass>       m_upsample;
    std::unique_ptr<SSRCompositePass>      m_composite;
    std::unique_ptr<SSRDepthHierarchyPass> m_depthHier;
    std::unique_ptr<SceneColorPyramidPass> m_sceneColorPyr;

    uint32_t m_frameIndex = 0;

    // ---- Transient scratch aliasing (OFF by default) -----------------------
    // When enabled, the resolve snapshot + color (both full-res RGBA16F, never
    // alive simultaneously) share one DEFAULT heap via CreatePlacedResource +
    // aliasing barriers, reclaiming ~one full-res RGBA16F (~16MB @1440p). Flip
    // to true ONLY after validating with the D3D12 debug layer + GPU validation:
    // the aliasing + COMMON-state-reset contract (see Render) is runtime-checked
    // there, and a mistake is a GPU hang / corruption, not a compile error.
    static constexpr bool kSSREnableScratchAliasing = true;

    Microsoft::WRL::ComPtr<ID3D12Heap> m_aliasHeap;
    uint64_t m_aliasHeapSize = 0;
    uint32_t m_aliasHeapW = 0, m_aliasHeapH = 0;
    // Heaps retired on resolution change, freed only after their placed
    // resources' FrameCount-deep deferred release has drained (countdown).
    std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12Heap>, uint32_t>> m_retiredHeaps;

    // Ensure m_aliasHeap fits one full-res RGBA16F texture (snapshot + color
    // both placed at offset 0 — same size, lifetime-disjoint). Returns true if
    // usable. Recreates on resolution change, retiring the old heap.
    bool EnsureAliasHeap(GraphicsDX12& gfx, uint32_t w, uint32_t h);
    // Decrement retired-heap countdowns; release those whose placed resources
    // have drained. Call once per Render.
    void TickAliasHeap();
};
