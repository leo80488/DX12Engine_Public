#pragma once

// TAAPass — Temporal Anti-Aliasing resolve pass (compute).
//
// Reads:   current jittered HDR scene SRV, depth SRV, history buffer SRV.
// Writes:  resolved HDR texture (UAV, ping-pong).
//
// Usage:
//   1. Call SetFrameData() each frame before Execute().
//   2. Call SetHdrSrvHandle() / SetDepthSrvHandle() each frame.
//   3. Execute() dispatches the resolve and returns the output SRV handle.
//   4. Pass GetResolvedSrvHandle() to AutoExposure / Bloom / ToneMap.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class TAAPass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "TAAPass"; }
    void Setup(RG::RenderGraphBuilder&) override {}
    void Init(IGraphicsDevice& gfx)     override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // ---- Per-frame inputs (call before Execute) ----------------------------

    void SetHdrSrvHandle(uint64_t h)          { m_hdrSrvHandle      = h; }
    void SetDepthSrvHandle(uint64_t h)        { m_depthSrvHandle    = h; }
    void SetGBufferSrvHandle(uint64_t h)      { m_gbufferSrvHandle  = h; }
    void SetVelocitySrvHandle(uint64_t h)     { m_velocitySrvHandle = h; }
    void SetJitter(float jx, float jy)        { m_cbData.jitterX = jx; m_cbData.jitterY = jy; }
    void SetViewportSize(uint32_t w, uint32_t h) { m_vpW = w; m_vpH = h; }

    // Upload per-frame matrices and parameters.
    // invVP     : transpose(inverse(jitteredViewProj)) — row-vector convention.
    // prevVP    : transpose(unjitteredPrevViewProj)    — row-vector convention.
    // tauHistory: diffuse history time constant in seconds (e.g. 0.08).
    // hasHist   : false on first frame / after resize.
    // dt        : elapsed seconds this frame (for frame-rate-independent blend).
    void SetFrameData(const DirectX::XMFLOAT4X4& invVP,
                      const DirectX::XMFLOAT4X4& prevVP,
                      float tauHistory,
                      bool  hasHistory,
                      float deltaTime);

    // ---- Output ------------------------------------------------------------

    // GPU SRV handle of the current resolved buffer (valid after Execute()).
    // Pass this to AutoExposure / Bloom / ToneMap instead of the raw HDR handle.
    uint64_t GetResolvedSrvHandle() const;

    // ---- Resize handling ---------------------------------------------------
    // Called automatically when m_vpW/H changes relative to last Execute().
    void InvalidateHistory() { m_hasHistory = false; }

    // ---- Enable/disable ----------------------------------------------------
    // When disabled, Execute() returns immediately and downstream passes
    // read the raw HDR scene (Renderer skips the camera jitter too — see
    // BuildRenderScene — otherwise you'd get wobble without reconstruction).
    // Flipping back on invalidates history so reconstruction restarts fresh.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v)
    {
        if (v == m_enabled) return;
        m_enabled = v;
        if (v) m_hasHistory = false;
    }

    // Runtime tunables — read by Renderer when calling SetFrameData. Exposed
    // here so the editor panel can adjust without plumbing setters all over.
    float tauHistory            = 0.05f;  // diffuse history time constant (seconds)
    float colorBoxSigma         = 1.5f;   // base AABB gamma in HDR-LINEAR semantics. Falcor reference is 1.0; 1.5 reduces distant shimmer at the cost of slightly more ghost. Practical sweet spot per scene profiling: 1.2–1.3 (lower = sharper, more shimmer; higher = softer, more ghost). In TAA_USE_TONEMAP_BLEND mode the shader auto-rescales by 1.5× internally so this slider stays in linear semantics — see kTonemapSigmaScale in TAA.cs.hlsl.
    float colorBoxSigmaSpecular = 2.0f;   // AABB gamma for specular pixels — wider box keeps specular history alive
    float specularRoughnessMax  = 0.5f;   // roughness threshold for "specular" classifier
    bool  antiFlicker           = false;  // Falcor-style distance-to-clamp anti-flicker (Karis 2014)
    float velocityWiden         = 1.0f;   // motion-proportional AABB widening (Karis-dimming protection)
    float sharpenStrength       = 0.1f;   // Karis 5-tap unsharp blend factor (0 disables, 0.1 default; 0.2+ ringy)

private:
    // CPU-side CB mirror (must match TAACB in TAA_Common.hlsli exactly).
    struct alignas(16) TAACB
    {
        float    invViewProj[16];           // 64 bytes
        float    prevViewProj[16];          // 64 bytes
        uint32_t width;                     //  4 bytes
        uint32_t height;                    //  4 bytes
        float    tauHistory;                //  4 bytes — diffuse history time constant (seconds)
        float    hasHistory;                //  4 bytes
        float    deltaTime;                 //  4 bytes — elapsed seconds this frame
        float    jitterX;                   //  4 bytes — subpixel jitter X in pixels [-0.5, +0.5)
        float    jitterY;                   //  4 bytes — subpixel jitter Y in pixels [-0.5, +0.5)
        float    colorBoxSigma;             //  4 bytes — base AABB gamma
        float    colorBoxSigmaSpecular;     //  4 bytes — specular AABB gamma
        float    specularRoughnessMax;      //  4 bytes — specular threshold
        uint32_t antiFlicker;               //  4 bytes — 0/1 toggle for Falcor distance-to-clamp formula
        float    velocityWiden;             //  4 bytes — motion-proportional AABB widening factor (Fix L)
        float    sharpenStrength;           //  4 bytes — Karis unsharp blend factor (#4)
        float    _pad0;                     //  4 bytes — 16-byte alignment to match TAACB layout
    };

    void RebuildBuffers();

    IGraphicsDevice*    m_gfx       = nullptr;
    RHI::PipelineState  m_pso;
    ShaderLibrary       m_shaderLib;

    // Ping-pong history buffers (R16G16B16A16_FLOAT).
    RHI::Texture        m_pingPong[2];
    int                 m_writeIdx  = 0;   // index of the write (current) buffer
    bool                m_hasHistory = false;

    // Resolved buffer's state tracker (alternates between UAV and SRV).
    RHI::ResourceState  m_resolvedState{};

    // History buffer's state (starts as SRV after first copy).
    RHI::ResourceState  m_historyState{};

    // Persistently mapped constant buffer.
    RHI::GPUBuffer m_cb;
    void*          m_cbMapped = nullptr;

    // Per-frame data set by Renderer.
    uint64_t m_hdrSrvHandle      = 0;
    uint64_t m_depthSrvHandle    = 0;
    uint64_t m_gbufferSrvHandle  = 0;
    uint64_t m_velocitySrvHandle = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;
    uint32_t m_lastVpW = 0;
    uint32_t m_lastVpH = 0;

    TAACB    m_cbData{};
    bool     m_enabled = true;
};
