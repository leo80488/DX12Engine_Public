#pragma once

// AutoExposurePass — histogram-based auto exposure.
//
// Two compute dispatches per frame:
//   1. CSHistogramBuild   — accumulates 256-bin log-luma histogram from HDR scene.
//   2. CSHistogramAverage — computes adapted exposure value from histogram.
//
// Exposure is stored in a 1-element UPLOAD/DEFAULT buffer read by ToneMapPass.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class AutoExposurePass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "AutoExposurePass"; }
    void Setup  (RG::RenderGraphBuilder&) override {}
    void Init   (IGraphicsDevice& gfx)   override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // Set per-frame inputs.
    void SetHdrSrvHandle(uint64_t handle)      { m_hdrSrvHandle = handle; }
    void SetViewportSize(uint32_t w, uint32_t h) { m_vpW = w; m_vpH = h; }

    // Master enable. When false, Execute() skips histogram dispatches and
    // uploads `m_manualExposure` to the exposure buffer instead — downstream
    // ToneMapPass is oblivious (it always reads the same SRV).
    void  SetEnabled(bool e)         { m_enabled = e; }
    bool  IsEnabled()          const { return m_enabled; }
    // Fixed exposure used when disabled. 1.0 = no scaling.
    void  SetManualExposure(float e) { m_manualExposure = e; }
    float GetManualExposure()  const { return m_manualExposure; }

    // Tuning parameters (optional; defaults are reasonable).
    void SetAdaptationRate(float r)   { m_adaptationRate = r; }
    void SetLuminanceRange(float lo, float hi) { m_minLogLuma = lo; m_maxLogLuma = hi; }
    void SetPercentiles(float lo, float hi)   { m_lowPercent = lo; m_highPercent = hi; }
    void SetMinExposure(float e)              { m_minExposure = e; }
    void SetMaxExposure(float e)              { m_maxExposure = e; }
    // EV compensation (stops). +1 → 2× brighter, −1 → 2× darker.
    // Handy when a super-bright IBL sky forces AE to crush subject exposure.
    void  SetEVBias(float ev)        { m_evBias = ev; }
    float GetEVBias() const          { return m_evBias; }
    // Middle-grey key target (default 0.18). Larger = brighter auto-exposure.
    void  SetKeyValue(float k)       { m_keyValue = k; }
    float GetKeyValue() const        { return m_keyValue; }
    float GetLowPercent()  const     { return m_lowPercent; }
    float GetHighPercent() const     { return m_highPercent; }
    float GetMinExposure() const     { return m_minExposure; }
    float GetMaxExposure() const     { return m_maxExposure; }

    // Returns the exposure buffer — Renderer passes it to gfx.GetBufferSRVGpuHandle().
    const RHI::GPUBuffer& GetExposureBuffer() const { return m_exposureBuffer; }

private:
    IGraphicsDevice* m_gfx = nullptr;
    RHI::PipelineState m_buildPSO;
    RHI::PipelineState m_averagePSO;
    ShaderLibrary       m_shaderLib;

    // 256-element histogram (uint) — UAV only
    RHI::GPUBuffer m_histogramBuffer;
    // 1-element exposure (float) — UAV + SRV
    RHI::GPUBuffer m_exposureBuffer;
    RHI::ResourceState m_exposureState = RHI::ResourceState::UNDEFINED;
    // Staging buffer (UPLOAD) used to push `m_manualExposure` into
    // m_exposureBuffer when the pass is disabled. Persistently mapped.
    // Triple-buffered — Execute() writes one slot per frame, then CopyBuffer
    // queues a GPU read of that same slot; a single buffer would race the
    // pending copy when CPU runs ahead.
    static constexpr uint32_t kFrameCount = 3;
    RHI::GPUBuffer m_manualStaging[kFrameCount];
    void*          m_manualStagingMapped[kFrameCount] = {};

    // Per-dispatch CB
    struct alignas(16) AutoExposureCB
    {
        uint32_t width;
        uint32_t height;
        float    minLogLuma;
        float    invLogLumaRange;
        float    adaptationRate;
        float    lowPercent;
        float    highPercent;
        float    minExposure;
        float    maxExposure;
        float    evBias;
        float    keyValue;
        float    pad[2];
    };
    FrameCB<AutoExposureCB> m_cb;

    uint64_t m_hdrSrvHandle = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;

    float m_adaptationRate = 0.02f;  // set per-frame by Renderer via SetAdaptationRate()
    float m_minLogLuma     = -5.0f;
    float m_maxLogLuma     =  3.5f;
    float m_lowPercent     = 0.50f;
    float m_highPercent    = 0.85f;
    float m_minExposure    = 0.10f;
    float m_maxExposure    = 8.0f;
    float m_evBias         = 0.0f;   // exposure compensation in EV stops
    float m_keyValue       = 0.18f;  // middle-grey target

    bool  m_enabled        = true;
    float m_manualExposure = 1.0f;   // used when !m_enabled
};
