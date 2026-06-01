#pragma once

// FXAAPass — NVIDIA FXAA 3.11 quality preset, single-pass HDR-aware compute.
//
// Reads:   one HDR scene SRV (raw HDR, or TAA's resolved output for FXAA+TAA).
// Writes:  one ping-pong R16G16B16A16_FLOAT output.
// No history, no jitter — purely spatial.
//
// Usage:
//   1. Call SetInputSrvHandle() each frame with the source SRV.
//   2. Call SetViewportSize() if viewport changed.
//   3. Execute() dispatches the resolve.
//   4. Pass GetResolvedSrvHandle() to downstream (Bloom / AutoExposure / ToneMap).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class FXAAPass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "FXAAPass"; }
    void Setup(RG::RenderGraphBuilder&) override {}
    void Init(IGraphicsDevice& gfx)     override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // ---- Per-frame inputs --------------------------------------------------
    void SetInputSrvHandle(uint64_t h)           { m_inputSrvHandle = h; }
    void SetViewportSize(uint32_t w, uint32_t h) { m_vpW = w; m_vpH = h; }

    // ---- Output ------------------------------------------------------------
    uint64_t GetResolvedSrvHandle() const;

    // ---- Enable / runtime tunables ----------------------------------------
    bool IsEnabled() const  { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // FXAA 3.11 quality knobs — exposed so editor can tune without recompiling.
    // qualitySubpix         : 0 disables sub-pixel AA; 0.75 default; 1.0 max softening
    // qualityEdgeThreshold  : minimum local contrast to apply AA. 0.063 high quality,
    //                         0.166 default, 0.333 low quality (skip more pixels)
    // qualityEdgeThresholdMin: absolute floor. 0.0312 high, 0.0833 default
    float qualitySubpix          = 0.75f;
    float qualityEdgeThreshold   = 0.166f;
    float qualityEdgeThresholdMin= 0.0833f;

private:
    struct alignas(16) FXAACB
    {
        uint32_t width;
        uint32_t height;
        float    qualitySubpix;
        float    qualityEdgeThreshold;
        float    qualityEdgeThresholdMin;
        float    _pad0;
        float    _pad1;
        float    _pad2;
    };

    void RebuildBuffers();

    IGraphicsDevice*    m_gfx = nullptr;
    RHI::PipelineState  m_pso;
    ShaderLibrary       m_shaderLib;

    RHI::Texture        m_output;
    RHI::ResourceState  m_outputState{};

    FrameCB<FXAACB>     m_cb;

    uint64_t m_inputSrvHandle = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;
    uint32_t m_lastVpW = 0;
    uint32_t m_lastVpH = 0;

    FXAACB   m_cbData{};
    bool     m_enabled = false;
};
