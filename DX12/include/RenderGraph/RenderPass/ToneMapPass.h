#pragma once

// ToneMapPass — ACES tone mapping + bloom composite + color grading LUT.
//
// Reads:  HDR scene (SRV), bloom[0] (SRV), exposure buffer (SRV), LUT 3D (SRV).
// Writes: LDR finalOutput texture (UAV, RGBA8 UNORM).
//
// The pass owns a 32^3 3D LUT texture.  When ColorGradingParams change (dirty),
// a compute dispatch bakes the LUT before the tonemap dispatch.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "RenderGraph/RenderPass/ColorGradingParams.h"

class ToneMapPass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "ToneMapPass"; }
    void Setup  (RG::RenderGraphBuilder&) override {}
    void Init   (IGraphicsDevice& gfx)   override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // Set per-frame inputs.
    void SetHdrSrvHandle(uint64_t handle)          { m_hdrSrvHandle = handle; }
    void SetBloomSrvHandle(uint64_t handle)        { m_bloomSrvHandle = handle; }
    void SetExposureSrvHandle(uint64_t handle)     { m_exposureSrvHandle = handle; }
    void SetLensFlareSrvHandle(uint64_t handle)    { m_lensFlareSrvHandle = handle; }
    void SetViewportSize(uint32_t w, uint32_t h)   { m_vpW = w; m_vpH = h; }

    void  SetLensFlareStrength(float v) { m_lensFlareStrength = v; }
    float GetLensFlareStrength() const  { return m_lensFlareStrength; }

    void EnsureTexture(uint32_t w, uint32_t h)
    {
        m_vpW = w; m_vpH = h;
        if (m_vpW != m_lastVpW || m_vpH != m_lastVpH)
            RebuildTexture();
    }

    float GetBloomStrength() const      { return m_bloomStrength; }
    void  SetBloomStrength(float v)     { m_bloomStrength = v; }

    // --- Color Grading ---
    void SetColorGradingParams(const ColorGradingParams& p)
    {
        if (p != m_gradingParams) { m_gradingParams = p; m_lutDirty = true; }
    }
    const ColorGradingParams& GetColorGradingParams() const { return m_gradingParams; }
    bool  IsColorGradingEnabled() const { return m_colorGradingEnabled; }
    void  SetColorGradingEnabled(bool v){ m_colorGradingEnabled = v; m_lutDirty = true; }

    // SRV handle of the final LDR output texture.
    uint64_t GetFinalOutputSrvHandle() const;

    const RHI::Texture* GetFinalOutputTexture() const { return &m_finalOutput; }

    void TransitionForDisplay(RHI::CommandList& cl);
    void PrepareForCompute(RHI::CommandList& cl);

    // Out-of-pass barrier coordination — UIPass / overlays read the current
    // tracked state to emit matching transitions, then write back the state
    // they leave the texture in so subsequent passes see the right "before".
    RHI::ResourceState GetFinalOutputState() const { return m_finalOutputState; }
    void               SetFinalOutputState(RHI::ResourceState s) { m_finalOutputState = s; }

private:
    // Tonemap PSO
    RHI::PipelineState m_pso;

    // LUT bake PSO
    RHI::PipelineState m_lutPso;

    ShaderLibrary m_shaderLib;

    // Final LDR output texture (RGBA8 UNORM, SRV + UAV)
    RHI::Texture        m_finalOutput;
    RHI::ResourceState  m_finalOutputState{};
    uint32_t            m_lastVpW = 0;
    uint32_t            m_lastVpH = 0;

    // 3D LUT texture (32^3, R16G16B16A16_FLOAT, SRV + UAV)
    RHI::Texture        m_lutTexture;
    RHI::ResourceState  m_lutState{};

    // Per-dispatch CB (tonemap)
    struct alignas(16) ToneMapCB
    {
        uint32_t width;
        uint32_t height;
        float    bloomStrength;
        uint32_t enableLUT;
        float    lensFlareStrength;
        float    _pad0;
        float    _pad1;
        float    _pad2;
    };
    FrameCB<ToneMapCB> m_cb;

    // Per-dispatch CB (LUT bake)
    RHI::GPUBuffer m_gradingCb;
    void*          m_gradingCbMapped = nullptr;

    // Color grading state
    ColorGradingParams m_gradingParams;
    bool               m_lutDirty            = true;
    bool               m_colorGradingEnabled = true;

    uint64_t m_hdrSrvHandle       = 0;
    uint64_t m_bloomSrvHandle     = 0;
    uint64_t m_exposureSrvHandle  = 0;
    uint64_t m_lensFlareSrvHandle = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;

    float m_bloomStrength     = 0.04f;
    float m_lensFlareStrength = 1.0f;

    IGraphicsDevice* m_gfx = nullptr;

    void RebuildTexture();
    void CreateLUT();
    void BakeLUT(RHI::CommandList cl);
};
