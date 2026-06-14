#pragma once

// UnderwaterPass — underwater screen distortion + water tint (compute, HDR).
//
// Modeled on CASPass: an HDR-space producer that reads an input SRV and writes
// a same-size R16G16B16A16_FLOAT output, which the PostProcess::Stack swaps in
// as the running ctx.hdrSrv (so tonemap sees the distorted frame). Driven by
// the `underwater` group of ResolvedPostProcessSettings via UnderwaterEffect.
//
// `time` is accumulated internally (AddTime each frame) so the wobble animates
// without the caller threading a clock through.

#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <DirectXMath.h>

class IGraphicsDevice;

class UnderwaterPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Per-frame inputs.
    void SetInputSrv(uint64_t srv)               { m_inputSrv = srv; }
    void SetViewportSize(uint32_t w, uint32_t h);
    void AddTime(float dt)                        { m_time += dt; }

    void SetEnabled(bool v)   { m_enabled = v; }
    bool IsEnabled() const     { return m_enabled; }

    // Look parameters (pushed by the adapter from the resolved profile).
    float             strength   = 0.012f;  // UV-space wobble amplitude
    float             scale      = 28.0f;   // wave spatial frequency
    float             speed      = 1.5f;    // wave temporal speed
    float             tintAmount = 0.6f;    // 0..1 blend toward tint
    DirectX::XMFLOAT3 tint       = { 0.45f, 0.75f, 0.85f }; // water colour

    void Execute(RHI::CommandList cl);

    // SRV of the distorted HDR result (valid after Execute).
    uint64_t GetOutputSrvHandle() const;

private:
    void RebuildTextures();

    struct alignas(16) UnderwaterCB
    {
        uint32_t width;
        uint32_t height;
        float    time;
        float    strength;

        float    scale;
        float    speed;
        float    tintAmount;
        float    _pad0;

        float    tint[3];
        float    _pad1;
    };

    IGraphicsDevice*    m_gfx = nullptr;
    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;
    FrameCB<UnderwaterCB> m_cb;

    RHI::Texture       m_output;
    RHI::ResourceState m_outputState = RHI::ResourceState::UNORDERED_ACCESS;

    uint64_t m_inputSrv = 0;
    uint32_t m_vpW = 0, m_vpH = 0;
    float    m_time = 0.0f;
    bool     m_texDirty = true;
    bool     m_enabled  = false;   // off until a volume/profile enables it
};
