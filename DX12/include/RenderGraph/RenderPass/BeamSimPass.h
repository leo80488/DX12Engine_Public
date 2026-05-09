#pragma once

// BeamSimPass — runs the beam-tube generation compute shader once per
// active beam each frame. Standalone (manual BeginCommandList) like
// ParticleSimPass and SkinningPass — outputs UAV buffers that downstream
// graphics passes (GBuffer / Transparent) read as PVF SRVs.

#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

class IGraphicsDevice;
class BeamSystem;

class BeamSimPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) { (void)gfx; }

    void SetSystem(BeamSystem* sys) { m_sys = sys; }

    void Execute(RHI::CommandList cl);

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_genPSO;

    BeamSystem*        m_sys = nullptr;
};
