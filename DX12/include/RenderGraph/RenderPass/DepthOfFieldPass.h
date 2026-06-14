#pragma once

// DepthOfFieldPass — focus-distance depth of field (compute, HDR).
//
// Like CASPass/UnderwaterPass it is an HDR-space producer: reads the running
// HDR colour (t0) + the hardware scene depth (t1, reverse-Z), writes a blurred
// R16G16B16A16_FLOAT output that the PostProcess::Stack swaps in as ctx.hdrSrv.
// Driven by the `dof` group of ResolvedPostProcessSettings via DepthOfFieldEffect.

#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;

class DepthOfFieldPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Per-frame inputs.
    void SetInputSrv(uint64_t srv)               { m_inputSrv = srv; }
    void SetDepthSrv(uint64_t srv)               { m_depthSrv = srv; }
    void SetCameraPlanes(float nearZ, float farZ){ m_near = nearZ; m_far = farZ; }
    void SetViewportSize(uint32_t w, uint32_t h);

    void SetEnabled(bool v) { m_enabled = v; }
    bool IsEnabled() const   { return m_enabled; }

    // Look parameters (pushed by the adapter from the resolved profile).
    float focusDistance   = 8.0f;   // world units kept sharp
    float focusRange      = 2.0f;   // half-width of the in-focus band
    float transitionRange = 10.0f;  // ramp distance past the band
    float maxRadius       = 8.0f;   // max blur radius in pixels (aperture proxy)

    void Execute(RHI::CommandList cl);

    // SRV of the blurred HDR result (valid after Execute).
    uint64_t GetOutputSrvHandle() const;

private:
    void RebuildTextures();

    struct alignas(16) DoFCB
    {
        uint32_t width;
        uint32_t height;
        float    nearZ;
        float    farZ;

        float    focusDistance;
        float    focusRange;
        float    transitionRange;
        float    maxRadius;
    };

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<DoFCB>     m_cb;

    RHI::Texture       m_output;
    RHI::ResourceState m_outputState = RHI::ResourceState::UNORDERED_ACCESS;

    uint64_t m_inputSrv = 0;
    uint64_t m_depthSrv = 0;
    float    m_near = 0.1f;
    float    m_far  = 1000.0f;
    uint32_t m_vpW = 0, m_vpH = 0;
    bool     m_texDirty = true;
    bool     m_enabled  = false;
};
