#pragma once

// SSRPass (Hi-Z trace) — Pass 2 of the Hi-Z SSR pipeline.
//
// Stochastic GGX ray gen + FidelityFX-SSSR-style Hi-Z traversal, with a
// Lumen-style pixel-precision linear finish trace (Phase 2) so converged hit
// UVs land on the actual mesh pixel rather than a Hi-Z cell corner.
//
// Phase 3: dispatch runs at HALF-RES with per-frame sub-pixel jitter; output
// textures are half-resolution. Combined with temporal accumulation, four
// consecutive frames reconstruct native-res detail.
//
// Output textures (all R16G16B16A16_FLOAT / R16F at half render res):
//   hitBuffer    .rgb = scene color sampled at hit UV, .a = confidence
//   rayDirPDF    .rgb = world-space reflection direction L, .a = PDF
//   rayLength    world-space ray distance (0 on miss)

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRPass
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot-reload entry point: re-fetches the trace CS from disk and rebuilds
    // the compute PSO. Idempotent; safe to call from a filesystem watcher.
    void ReloadShaders(IGraphicsDevice& gfx);

    // (Re)allocate output textures. @p renderW/@p renderH are FULL-resolution
    // dims; internal trace textures are sized at half-res.
    void EnsureTexture(uint32_t renderW, uint32_t renderH);

    struct Camera
    {
        DirectX::XMFLOAT4X4 viewProj;     // jittered
        DirectX::XMFLOAT4X4 invViewProj;  // jittered inverse
        DirectX::XMFLOAT3   cameraPos;
        float               nearZ;
        float               farZ;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }
    void SetFrameIndex(uint32_t f)    { m_frameIndex = f; }
    void SetHiZMipCount(uint32_t m)   { m_hizMipCount = m; }

    void SetTraceParams(float thickness, uint32_t hizMostDetailed,
                        float coneMipMax, float depthBiasFactor)
    {
        m_traceThickness    = thickness;
        m_hizMostDetailed   = hizMostDetailed;
        m_coneMipMax        = coneMipMax;
        m_depthBiasFactor   = depthBiasFactor;
    }
    void SetMaxRayLength(float v)         { m_maxRayLength = v; }
    void SetRoughnessCutoff(float v)      { m_roughnessCutoff = v; }
    void SetFinishLinearSteps(uint32_t n) { m_finishLinearSteps = n; }

    float    GetTraceThickness()    const { return m_traceThickness; }
    uint32_t GetHiZMostDetailed()   const { return m_hizMostDetailed; }
    float    GetConeMipMax()        const { return m_coneMipMax; }
    float    GetDepthBiasFactor()   const { return m_depthBiasFactor; }
    float    GetMaxRayLength()      const { return m_maxRayLength; }
    float    GetRoughnessCutoff()   const { return m_roughnessCutoff; }
    uint32_t GetFinishLinearSteps() const { return m_finishLinearSteps; }

    void Execute(RHI::CommandList cl,
                 uint64_t normalSrv,
                 uint64_t surfaceSrv,
                 uint64_t depthSrv,
                 uint64_t depthHierSrv,
                 uint64_t hdrPyramidSrv,
                 uint64_t velocitySrv);

    uint64_t GetResultSrv()     const;
    uint64_t GetRayDirPDFSrv()  const;
    uint64_t GetRayLengthSrv()  const;

    const RHI::Texture* GetResultTexture()    const { return &m_resultTex; }
    const RHI::Texture* GetRayDirPDFTexture() const { return &m_rayDirPDFTex; }
    const RHI::Texture* GetRayLengthTexture() const { return &m_rayLengthTex; }

    void SetResultState(RHI::ResourceState s)    { m_resultState    = s; }
    void SetRayDirPDFState(RHI::ResourceState s) { m_rayDirPDFState = s; }
    void SetRayLengthState(RHI::ResourceState s) { m_rayLengthState = s; }
    void SetAllOutputsState(RHI::ResourceState s)
    { m_resultState = m_rayDirPDFState = m_rayLengthState = s; }
    RHI::ResourceState GetResultState() const { return m_resultState; }

    uint32_t GetWidth()        const { return m_w; }
    uint32_t GetHeight()       const { return m_h; }
    uint32_t GetRenderWidth()  const { return m_renderW; }
    uint32_t GetRenderHeight() const { return m_renderH; }

    struct alignas(16) SSRTraceCB
    {
        float    viewProj[16];
        float    invViewProj[16];
        float    cameraPos[3];        float    nearZ;
        // renderW/H = full-res GBuffer dims; traceW/H = half-res dispatch.
        uint32_t renderW;             uint32_t renderH;
        uint32_t hizMipCount;         float    maxRayLength;
        float    farZ;                float    roughnessCutoff;
        uint32_t frameIndex;          uint32_t _pad1;
        float    traceThickness;      uint32_t hizMostDetailedLvl;
        float    coneMipMax;          float    depthBiasFactor;
        uint32_t finishLinearSteps;   uint32_t traceW;
        uint32_t traceH;              uint32_t _pad2[1];
    };

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    RHI::Texture       m_resultTex;     // RGBA16F: hit color + confidence
    RHI::Texture       m_rayDirPDFTex;  // RGBA16F: world L + PDF
    RHI::Texture       m_rayLengthTex;  // R16F: world ray distance
    RHI::ResourceState m_resultState     = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_rayDirPDFState  = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_rayLengthState  = RHI::ResourceState::UNORDERED_ACCESS;
    uint32_t           m_w = 0, m_h = 0;          // trace (half) dims
    uint32_t           m_renderW = 0, m_renderH = 0;

    FrameCB<SSRTraceCB> m_cb;

    Camera   m_cam{};
    uint32_t m_frameIndex   = 0;
    uint32_t m_hizMipCount  = 1;

    // Used ONLY on the LinearFinishTrace fall-through path (budget-exhausted
    // rays that didn't find a clean crossing). Successful finish-trace hits
    // skip this check entirely — the snap is sub-pixel-precise so thickness
    // would always pass anyway. 0.5 wu is a safe fall-through default; tight
    // values rejected legitimate "grazing" hits that the forward walk failed
    // to converge on within 16 steps.
    float    m_traceThickness     = 0.5f;
    uint32_t m_hizMostDetailed    = 0;
    float    m_coneMipMax         = 4.0f;
    float    m_depthBiasFactor    = 0.005f;
    float    m_maxRayLength       = 100.0f;
    float    m_roughnessCutoff    = 0.5f;
    // Finish trace refines from the Hi-Z hit position with ±maxSteps/2
    // backward then maxSteps forward 1-pixel walks. With Hi-Z lands within
    // ~1 cell of the true crossing, 16 is plenty; only crank if Hi-Z is
    // suspected to overshoot more than that.
    uint32_t m_finishLinearSteps  = 16;
};
