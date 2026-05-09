#pragma once

// SSRPass — Pass 2 of Hi-Z SSR pipeline: stochastic ray gen + Hi-Z traversal.
//
// Writes a per-pixel HIT BUFFER that downstream passes (spatial resolve,
// temporal) consume. Owned directly by Renderer (not registered with
// RenderGraph) because it sits in the post-graph Phase 4.6 slot where
// HiZ lives, and it consumes already-written GBuffer state.
//
// Output layout (R16G16B16A16_FLOAT, render resolution):
//   .xy = hit UV in screen space       (0 on miss)
//   .z  = hit depth (NDC, reverse-Z)   (0 on miss)
//   .w  = 1/pdf                        (0 on miss — doubles as mask)
//
// Use hit.w > 0 as the miss test downstream.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // (Re)allocate the output texture when render resolution changes. Keeps
    // the existing resource when dimensions are unchanged.
    void EnsureTexture(uint32_t w, uint32_t h);

    // Per-frame camera state. MUST be the JITTERED viewProj / invViewProj
    // so the reconstruction matches the depth buffer rasterization and the
    // LightingPass's own depth→worldPos math — otherwise hit samples land on
    // the wrong pixels by the TAA sub-pixel jitter amount.
    struct Camera
    {
        DirectX::XMFLOAT4X4 viewProj;     // jittered
        DirectX::XMFLOAT4X4 invViewProj;  // jittered inverse
        DirectX::XMFLOAT3   cameraPos;
        float               nearZ;
        float               farZ;        // needed for reverse-Z linearisation
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }
    void SetFrameIndex(uint32_t f)    { m_frameIndex = f; }
    void SetHiZMipCount(uint32_t m)   { m_hizMipCount = m; }

    // UE-aligned trace knobs (see SSRTrace.cs.hlsl for semantics).
    //   thickness         — linear-Z tolerance for hit validation (world u.)
    //   hizMostDetailed   — finest pyramid mip the walker descends to
    //   coneMipMax        — upper bound on pre-filtered scene-color mip
    //   depthBiasFactor   — fractional linear-Z bias to push origin toward
    //                       camera, avoids Hi-Z self-intersection. 0 disables.
    void SetTraceParams(float thickness, uint32_t hizMostDetailed,
                        float coneMipMax, float depthBiasFactor)
    {
        m_traceThickness    = thickness;
        m_hizMostDetailed   = hizMostDetailed;
        m_coneMipMax        = coneMipMax;
        m_depthBiasFactor   = depthBiasFactor;
    }
    void SetMaxRayLength(float v)    { m_maxRayLength = v; }
    void SetRoughnessCutoff(float v) { m_roughnessCutoff = v; }

    // Read-back accessors for editor tuning UI.
    float    GetTraceThickness()  const { return m_traceThickness; }
    uint32_t GetHiZMostDetailed() const { return m_hizMostDetailed; }
    float    GetConeMipMax()      const { return m_coneMipMax; }
    float    GetDepthBiasFactor() const { return m_depthBiasFactor; }
    float    GetMaxRayLength()    const { return m_maxRayLength; }
    float    GetRoughnessCutoff() const { return m_roughnessCutoff; }

    // Dispatch the trace. All input SRVs must be in SHADER_RESOURCE; caller
    // owns those transitions. All three output textures stay in UAV between
    // dispatches — callers consuming them must transition to SR themselves.
    //
    //   depthHierSrv  : 2-channel depth pyramid (SSRDepthHierarchyPass)
    //   hdrPyramidSrv : pre-filtered HDR pyramid (SceneColorPyramidPass)
    //   velocitySrv   : NDC motion vector (GBuffer velocity target), may be 0
    void Execute(RHI::CommandList cl,
                 uint64_t normalSrv,
                 uint64_t surfaceSrv,
                 uint64_t depthSrv,
                 uint64_t depthHierSrv,
                 uint64_t hdrPyramidSrv,
                 uint64_t velocitySrv);

    // SRVs of the three outputs. Handles are stable across frames (textures
    // re-created only on resize).
    uint64_t GetResultSrv()     const;  // hitBuffer: color.rgb + conf.a
    uint64_t GetRayDirPDFSrv()  const;
    uint64_t GetRayLengthSrv()  const;

    const RHI::Texture* GetResultTexture()    const { return &m_resultTex; }
    const RHI::Texture* GetRayDirPDFTexture() const { return &m_rayDirPDFTex; }
    const RHI::Texture* GetRayLengthTexture() const { return &m_rayLengthTex; }

    // External state sync — call after any barrier outside the pass that
    // changes an output texture's state. Keeps the pass's internal trackers
    // honest so the next Execute picks the right `before` state.
    void SetResultState(RHI::ResourceState s)    { m_resultState    = s; }
    void SetRayDirPDFState(RHI::ResourceState s) { m_rayDirPDFState = s; }
    void SetRayLengthState(RHI::ResourceState s) { m_rayLengthState = s; }
    void SetAllOutputsState(RHI::ResourceState s)
    { m_resultState = m_rayDirPDFState = m_rayLengthState = s; }
    RHI::ResourceState GetResultState() const { return m_resultState; }

    uint32_t GetWidth()  const { return m_w; }
    uint32_t GetHeight() const { return m_h; }

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    // Trace output textures. All SRV+UAV, render resolution.
    //   m_resultTex   : RGBA16F — hit color + confidence (.a)
    //   m_rayDirPDFTex: RGBA16F — world-space L + sampled PDF (.a)
    //   m_rayLengthTex: R16F    — world-space ray distance (0 on miss)
    RHI::Texture       m_resultTex;
    RHI::Texture       m_rayDirPDFTex;
    RHI::Texture       m_rayLengthTex;
    RHI::ResourceState m_resultState     = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_rayDirPDFState  = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_rayLengthState  = RHI::ResourceState::UNORDERED_ACCESS;
    uint32_t           m_w = 0;
    uint32_t           m_h = 0;

    // Per-dispatch CB (UPLOAD, persistently mapped). One 256-byte slot only —
    // dispatch runs once per frame so no ring-buffer is needed.
    RHI::GPUBuffer m_cb;
    void*          m_cbMapped = nullptr;

    Camera   m_cam{};
    uint32_t m_frameIndex  = 0;
    uint32_t m_hizMipCount = 1;

    // Wicked-aligned default (1.5 world units). Earlier comment claimed UE
    // uses 0.02–0.05 — that was wrong for THIS engine's Hi-Z + linear-Z-diff
    // ValidateHit formula. Hi-Z's converged hit.z lands on the cell's max
    // depth, which can be tens of cm off the actual mesh surface (cells
    // aren't pixel-perfect at coarser mips). Tight 0.05 thresholds rejected
    // every legitimate mesh hit — a floor reflecting a mesh above showed
    // BLACK because every mesh-direction ray validated to confidence=0.
    // Diagnosed via a debug shader that painted the failure mode: rays
    // hitting where meshes should reflect came back BLUE = "validation
    // rejected" — the only fix consistent with the Wicked reference is to
    // open the thickness window to ~1.5m.
    //
    // Trade-off vs old 0.05: thin-surface tunneling (rays passing through
    // a thin wall to hit something behind it). Acceptable for typical
    // scenes; if foliage / fences become a problem, lower this to 0.5.
    float    m_traceThickness  = 1.5f;
    // MostDetailedLvl = 0 keeps Hi-Z cells at 1 pixel so adjacency with
    // closer geometry (box on floor) doesn't drag the starting cell's max
    // depth over origin.z — that was causing the walker to self-hit at
    // silhouette boundaries. Revisit if long-ray perf becomes the bottleneck.
    uint32_t m_hizMostDetailed = 0;
    float    m_coneMipMax      = 4.0f;  // was hardcoded 3 in shader
    // Without a bias, origin sits exactly on the reflecting surface and the
    // Hi-Z walker self-hits at iteration 0 → reflections show the plane's
    // own colour and objects touching the plane disappear. 0.5% is a safe
    // starting point; increase if you still see contact-point self-hits.
    float    m_depthBiasFactor  = 0.005f;
    float    m_maxRayLength     = 100.0f;
    float    m_roughnessCutoff  = 0.5f;
};

// ---------------------------------------------------------------------------
// SSRResolvePass — Pass 3: spatial BRDF reweight (no temporal any more).
// Owns the HDR snapshot (copied before dispatch to avoid HDR UAV aliasing)
// and three single-frame output textures:
//   color        RGBA16F  reflection (post-BRDF-reweight) + confidence
//   variance     R16F     Welford weighted variance of sample luminance
//   reprojDepth  R16F     NDC depth at the closest hit (temporal uses it
//                         for reflection-following reprojection)
// Temporal history management has moved to SSRTemporalPass.
// ---------------------------------------------------------------------------
class SSRResolvePass
{
public:
    void Init(IGraphicsDevice& gfx);
    void EnsureTextures(uint32_t w, uint32_t h);

    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3   cameraPos;
        float               nearZ;
        float               farZ;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }
    void SetFrameIndex(uint32_t f)    { m_frameIndex = f; }
    // Post-accumulation luminance cap. <=0 disables. UE-equivalent ~16.
    void SetFireflyCap(float cap)     { m_fireflyCap = cap; }
    float GetFireflyCap() const       { return m_fireflyCap; }

    // HDR snapshot (race-free copy consumed by the scene color pyramid).
    const RHI::Texture* GetSnapshotTexture() const { return &m_snapshotTex; }
    uint64_t            GetSnapshotSrv()     const;
    void PreSnapshotCopy(RHI::CommandList cl);
    void PostSnapshotCopy(RHI::CommandList cl);

    // Output textures — all in SHADER_RESOURCE state between frames.
    const RHI::Texture* GetColorTexture()       const { return &m_colorTex; }
    const RHI::Texture* GetVarianceTexture()    const { return &m_varianceTex; }
    const RHI::Texture* GetReprojDepthTexture() const { return &m_reprojDepthTex; }
    uint64_t GetColorSrv()       const;
    uint64_t GetVarianceSrv()    const;
    uint64_t GetReprojDepthSrv() const;

    // Inputs must be in SHADER_RESOURCE.
    //   hitBufferSrv : trace (color + conf)
    //   rayDirPDFSrv : trace (L + PDF)
    //   rayLengthSrv : trace (world-space ray distance)
    void Execute(RHI::CommandList cl,
                 uint32_t w, uint32_t h,
                 uint64_t normalSrv, uint64_t surfaceSrv, uint64_t depthSrv,
                 uint64_t hitBufferSrv, uint64_t rayDirPDFSrv,
                 uint64_t rayLengthSrv);

    uint32_t GetWidth()  const { return m_w; }
    uint32_t GetHeight() const { return m_h; }

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    RHI::GPUBuffer     m_cb;
    void*              m_cbMapped = nullptr;

    RHI::Texture       m_snapshotTex;
    RHI::ResourceState m_snapshotState = RHI::ResourceState::COPY_DST;

    RHI::Texture       m_colorTex;        // RGBA16F (SR+UAV)
    RHI::Texture       m_varianceTex;     // R16F    (SR+UAV)
    RHI::Texture       m_reprojDepthTex;  // R16F    (SR+UAV)
    RHI::ResourceState m_colorState       = RHI::ResourceState::SHADER_RESOURCE;
    RHI::ResourceState m_varianceState    = RHI::ResourceState::SHADER_RESOURCE;
    RHI::ResourceState m_reprojDepthState = RHI::ResourceState::SHADER_RESOURCE;

    uint32_t m_w = 0, m_h = 0;
    Camera   m_cam{};
    uint32_t m_frameIndex = 0;
    float    m_fireflyCap = 16.0f;
};

// ---------------------------------------------------------------------------
// SSRTemporalPass — Pass 4: dual-reprojection history accumulation.
// Reads resolve (color, variance, reprojDepth) + velocity + current depth.
// Maintains ping-pong history for color and variance plus a single prev-depth
// for next-frame disocclusion. Writes temporal color + temporal variance.
// ---------------------------------------------------------------------------
class SSRTemporalPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void EnsureTextures(uint32_t w, uint32_t h);

    struct Camera
    {
        // Reflection-following reprojection: BOTH matrices are JITTERED so
        // (a) inverse VP can correctly recover world position from the
        // jittered depth buffer, and (b) prev-frame projection lands UVs on
        // the previous frame's actual jittered pixel grid (= where history
        // was rendered). Mixing jittered current with non-jittered prev
        // (the earlier convention) injected ~1 px sub-pixel error every
        // frame; the EMA at temporalResponse=0.95 turned that into multi-
        // pixel ghost trails and a "reflection misaligned" static-frame
        // appearance.
        DirectX::XMFLOAT4X4 invViewProj;   // current inverse, jittered
        DirectX::XMFLOAT4X4 prevViewProj;  // previous-frame VP, jittered
        float               nearZ;
        float               farZ;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }
    void MarkReset()                  { m_resetHistory = true; }

    // Current frame's output SRVs (feed to upsample). These rotate between
    // m_color[0/1] / m_variance[0/1] each Execute().
    uint64_t GetColorSrv()    const;
    uint64_t GetVarianceSrv() const;
    const RHI::Texture* GetColorTexture()    const;
    const RHI::Texture* GetVarianceTexture() const;

    // Inputs in SHADER_RESOURCE.
    void Execute(RHI::CommandList cl,
                 uint32_t w, uint32_t h,
                 uint64_t colorCurrentSrv,
                 uint64_t varianceCurrentSrv,
                 uint64_t reprojDepthSrv,
                 uint64_t velocitySrv,
                 uint64_t depthSrv);

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    RHI::GPUBuffer     m_cb;
    void*              m_cbMapped = nullptr;

    RHI::Texture m_color[2];        // RGBA16F ping-pong
    RHI::Texture m_variance[2];     // R16F ping-pong
    // Ping-pong NDC reverse-Z depth. Each Execute() reads index `1-writeIdx`
    // (= last frame's depth) as gDepthHistory SRV and writes the CURRENT
    // frame's depth into index `writeIdx` via OutDepthHistory UAV. Used by
    // disocclusion detection so temporal accumulation actually converges.
    RHI::Texture m_depthHistory[2];

    uint32_t m_w = 0, m_h = 0;
    uint32_t m_writeIdx = 0;
    bool     m_resetHistory = true;
    Camera   m_cam{};
};

// ---------------------------------------------------------------------------
// SSRUpsamplePass — Pass 5: variance-driven bilateral blur.
// Reads temporal color + variance, produces the final SSR output consumed
// by LightingPass (1-frame latent dampening) and SSRComposite.
// ---------------------------------------------------------------------------
class SSRUpsamplePass
{
public:
    void Init(IGraphicsDevice& gfx);
    void EnsureTexture(uint32_t w, uint32_t h);

    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;
        float               nearZ;
        float               farZ;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }

    uint64_t GetColorSrv() const;
    const RHI::Texture* GetColorTexture() const { return &m_colorTex; }

    // Inputs in SHADER_RESOURCE.
    void Execute(RHI::CommandList cl,
                 uint32_t w, uint32_t h,
                 uint64_t temporalSrv, uint64_t varianceSrv,
                 uint64_t depthSrv, uint64_t normalSrv, uint64_t surfaceSrv);

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    RHI::GPUBuffer     m_cb;
    void*              m_cbMapped = nullptr;

    RHI::Texture m_colorTex;       // RGBA16F (SR+UAV)
    uint32_t m_w = 0, m_h = 0;
    Camera   m_cam{};
};

// ---------------------------------------------------------------------------
// SSRCompositePass — Pass 5: add resolved reflection into HDR.
// Runs as a separate compute dispatch after resolve finishes; reads GBuffer
// + resolved SSR + HDR UAV and additively blends the reflection in.
// Pairs with Lighting.ps's `iblSpecular *= (1 - ssrConf)` dampening so the
// combined result is energy-correct.
// ---------------------------------------------------------------------------
class SSRCompositePass
{
public:
    void Init(IGraphicsDevice& gfx);

    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3   cameraPos;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }

    // Global SSR intensity scale (debug / artist knob). Default 1.0 because
    // Lighting.ps dampens specIBL by (1-ssrConf) so the additive write is
    // physically-correct as-is. Tune away from 1.0 only for stylistic dimming.
    void  SetIntensity(float i)   { m_intensity = i; }
    float GetIntensity() const    { return m_intensity; }
    // Debug visualization mode. 0 = normal, 1 = SSR only, 2 = confidence
    // grayscale, 3 = hit UV as RG. Runtime-tunable from editor — no shader
    // recompile needed because it's a CB field not a permutation.
    void SetDebugMode(uint32_t m) { m_debugMode = m; }
    uint32_t GetDebugMode() const { return m_debugMode; }

    // Dispatch. HDR must be in UNORDERED_ACCESS state; GBuffer + ssrSrv +
    // BRDF SRVs must be in SHADER_RESOURCE.
    //
    // Debug SRVs (rawTraceSrv/rayDirSrv/rayLenSrv/varianceSrv) are only read
    // by debug modes 3..9; pass 0 if not routing them (those modes will show
    // black). All should be in SHADER_RESOURCE if non-zero.
    void Execute(RHI::CommandList cl,
                 uint32_t w, uint32_t h,
                 uint64_t albedoSrv, uint64_t normalSrv, uint64_t surfaceSrv,
                 uint64_t depthSrv,  uint64_t ssrSrv,    uint64_t hdrUav,
                 uint64_t brdfLutSrv,
                 uint64_t rawTraceSrv = 0, uint64_t rayDirSrv = 0,
                 uint64_t rayLenSrv   = 0, uint64_t varianceSrv = 0);

    // Tell composite what the current trace roughness cutoff is so debug mode
    // 8 (roughness mask) lines up with the trace's actual behaviour.
    void SetRoughnessCutoff(float v) { m_roughnessCutoff = v; }

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    RHI::GPUBuffer     m_cb;
    void*              m_cbMapped = nullptr;

    Camera   m_cam{};
    float    m_intensity       = 1.0f;
    uint32_t m_debugMode       = 0;
    float    m_roughnessCutoff = 0.5f;   // mirror of SSRPass::m_roughnessCutoff
};
