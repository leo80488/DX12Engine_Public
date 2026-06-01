#pragma once

// LensFlarePass — procedural directional-light lens flare.
//
// Reads the hardware depth (sun visibility / occlusion) and writes an additive
// HDR contribution into a half-resolution RGBA16F texture.  ToneMapPass binds
// this texture's SRV and adds it to the HDR scene before tonemapping, so the
// flare gets ACES-compressed alongside the rest of the frame.
//
// Sun position is supplied by the renderer via SetSunUV() — the sun direction
// is projected to screen-space on the CPU side using the camera's view-proj.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <DirectXMath.h>

class LensFlarePass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "LensFlarePass"; }
    void Setup  (RG::RenderGraphBuilder&) override {}
    void Init   (IGraphicsDevice& gfx)   override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // ---- Per-frame inputs (call before Execute) ----
    void SetEnabled       (bool v)       { m_enabled = v; }
    bool IsEnabled        () const       { return m_enabled; }

    void SetDepthSrvHandle(uint64_t h)   { m_depthSrv = h; }
    void SetDepthSourceSize(uint32_t w, uint32_t h) { m_depthW = w; m_depthH = h; }
    void SetViewportSize  (uint32_t w, uint32_t h)  { m_vpW = w; m_vpH = h; }

    void SetSun(const DirectX::XMFLOAT2& sunUV, bool sunBehind,
                const DirectX::XMFLOAT3& sunColor)
    {
        m_sunUV = sunUV; m_sunBehind = sunBehind; m_sunColor = sunColor;
    }

    // ---- Tunables (exposed so editor / volume system can drive them) ----
    void  SetIntensity      (float v) { m_intensity = v; }
    float GetIntensity      () const  { return m_intensity; }
    void  SetGhostCount     (uint32_t v) { m_ghostCount = v; }
    uint32_t GetGhostCount  () const  { return m_ghostCount; }
    void  SetGhostDispersal (float v) { m_ghostDispersal = v; }
    float GetGhostDispersal () const  { return m_ghostDispersal; }
    void  SetHaloWidth      (float v) { m_haloWidth = v; }
    float GetHaloWidth      () const  { return m_haloWidth; }
    void  SetStreakLength   (float v) { m_streakLength = v; }
    float GetStreakLength   () const  { return m_streakLength; }
    void  SetChromaticOffset(float v) { m_chromaticOffset = v; }
    float GetChromaticOffset() const  { return m_chromaticOffset; }

    // SRV GPU handle of the half-res RGBA16F flare texture.  Always valid after
    // Execute() — texture is cleared to black when the pass is disabled / sun
    // is behind the camera, so ToneMapPass can bind it unconditionally.
    uint64_t GetSrvHandle() const;
    const RHI::Texture* GetTexture() const { return &m_texture; }

private:
    RHI::PipelineState m_pso;
    ShaderLibrary      m_shaderLib;

    RHI::Texture       m_texture;          // half-res RGBA16F
    RHI::ResourceState m_textureState{};
    uint32_t           m_lastVpW = 0;
    uint32_t           m_lastVpH = 0;
    uint32_t           m_texW    = 0;
    uint32_t           m_texH    = 0;

    struct alignas(16) LensFlareCB
    {
        uint32_t dstWidth;
        uint32_t dstHeight;
        uint32_t srcDepthWidth;
        uint32_t srcDepthHeight;

        uint32_t enabled;
        float    intensity;
        float    sunBehind;
        float    chromaticOffset;

        float    sunUV[2];
        float    haloWidth;
        float    streakLength;

        float    sunColor[3];
        float    ghostDispersal;

        uint32_t ghostCount;
        float    streakWidth;
        float    occlusionRadius;
        float    _pad0;
    };
    FrameCB<LensFlareCB> m_cb;

    // Per-frame inputs
    bool                m_enabled    = true;
    uint64_t            m_depthSrv   = 0;
    uint32_t            m_depthW     = 0;
    uint32_t            m_depthH     = 0;
    uint32_t            m_vpW        = 0;
    uint32_t            m_vpH        = 0;
    DirectX::XMFLOAT2   m_sunUV      { 0.5f, 0.5f };
    bool                m_sunBehind  = true;
    DirectX::XMFLOAT3   m_sunColor   { 1.0f, 0.95f, 0.85f };

    // Tunables
    float    m_intensity       = 0.2f;
    uint32_t m_ghostCount      = 6;
    float    m_ghostDispersal  = 0.18f;
    float    m_haloWidth       = 0.32f;
    float    m_streakLength    = 0.55f;
    float    m_chromaticOffset = 0.012f;

    IGraphicsDevice* m_gfx = nullptr;

    void RebuildTexture();
    void DestroyTexture();
};
