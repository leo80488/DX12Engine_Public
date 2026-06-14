#pragma once

// StylizePass — NPR stylization (compute, HDR). One pass / one dispatch hosts
// five independently-toggled features (Kuwahara, Posterize, Dither, Halftone,
// Crosshatch) applied in NPR order. HDR-space producer like CASPass: reads the
// running HDR colour (t0), writes an output the Stack swaps into ctx.hdrSrv.
// Driven by the kuwahara/posterize/halftone/dither/crosshatch profile groups
// via StylizeEffect.

#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;

class StylizePass
{
public:
    void Init(IGraphicsDevice& gfx);

    void SetInputSrv(uint64_t srv)               { m_inputSrv = srv; }
    void SetViewportSize(uint32_t w, uint32_t h);
    void SetEnabled(bool v) { m_enabled = v; }   // "any feature on"
    bool IsEnabled() const   { return m_enabled; }

    // Feature parameters (pushed by the adapter from the resolved profile).
    bool     pixelateOn = false;  uint32_t pixelSize = 8;
    bool     kuwaharaOn = false;  uint32_t kuwaharaRadius = 3;
    bool     posterizeOn = false; uint32_t posterizeLevels = 6;
    bool     halftoneOn = false;  float    halftoneCell = 6.0f, halftoneAngle = 45.0f;
    bool     ditherOn = false;    uint32_t ditherLevels = 4;
    bool     crosshatchOn = false; float   crosshatchDensity = 0.12f, crosshatchThickness = 0.4f;

    void Execute(RHI::CommandList cl);

    uint64_t GetOutputSrvHandle() const;

private:
    void RebuildTextures();

    struct alignas(16) StylizeCB
    {
        uint32_t width;
        uint32_t height;
        uint32_t kuwaharaOn;
        uint32_t posterizeOn;

        uint32_t halftoneOn;
        uint32_t ditherOn;
        uint32_t crosshatchOn;
        uint32_t kuwaharaRadius;

        float    posterizeLevels;
        float    halftoneCell;
        float    halftoneAngle;
        float    ditherLevels;

        float    crosshatchDensity;
        float    crosshatchThickness;
        uint32_t pixelateOn;
        uint32_t pixelSize;
    };

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<StylizeCB> m_cb;

    RHI::Texture       m_output;
    RHI::ResourceState m_outputState = RHI::ResourceState::UNORDERED_ACCESS;

    uint64_t m_inputSrv = 0;
    uint32_t m_vpW = 0, m_vpH = 0;
    bool     m_texDirty = true;
    bool     m_enabled  = false;
};
