#pragma once

// CASPass — AMD FidelityFX Contrast Adaptive Sharpening (compute).
//
// Inserted between TAA (HDR resolve) and AutoExposure/Bloom/ToneMap so the
// sharpening is applied in HDR space and propagates naturally through the
// rest of the post chain. If disabled or unavailable, Renderer routes the
// raw/TAA-resolved HDR straight through.
//
// The operation is in-place-equivalent via a dedicated output texture:
//   HDR input  (R16G16B16A16_FLOAT, read as SRV)
//   HDR output (R16G16B16A16_FLOAT, same size, written as UAV)

#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;

class CASPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Per-frame inputs.
    void SetInputSrv(uint64_t srv)            { m_inputSrv = srv; }
    void SetViewportSize(uint32_t w, uint32_t h);

    // Record the compute dispatch onto cl.
    void Execute(RHI::CommandList cl);

    // Public SRV handle of the sharpened HDR result (valid after Execute).
    uint64_t GetOutputSrvHandle() const;

    // Enable/disable — Renderer checks this before inserting CAS into the
    // post chain. Disabling doesn't free GPU resources (cheap toggle).
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // Sharpness [0..1] — matches AMD CAS convention. Note that even at 0
    // there's a mild sharpen (peak = -1/8) because the weight curve
    // interpolates between -1/8 and -1/5 — so 0 is "subtle" rather than
    // fully off. Use SetEnabled(false) for a true passthrough.
    //   0.3-0.5 → TAA-softness cleanup (quality preset)
    //   0.6-0.8 → visible crispening (sharpen preset)
    //   0.9-1.0 → aggressive; can ring on specular edges in HDR
    float sharpness = 0.6f;

private:
    void RebuildTextures();

    struct alignas(16) CASCB
    {
        uint32_t width;
        uint32_t height;
        float    sharpness;
        float    _pad;
    };

    IGraphicsDevice*    m_gfx = nullptr;
    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;

    FrameCB<CASCB>      m_cb;

    RHI::Texture       m_output;
    RHI::ResourceState m_outputState = RHI::ResourceState::UNORDERED_ACCESS;

    uint64_t m_inputSrv = 0;
    uint32_t m_vpW = 0, m_vpH = 0;
    bool     m_texDirty = true;
    bool     m_enabled  = true;
};
