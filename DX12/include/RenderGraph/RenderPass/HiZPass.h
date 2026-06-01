#pragma once

// HiZPass — Hierarchical-Z mip chain generation.
//
// Reads the GBuffer depth buffer and generates a min-depth mip chain
// for GPU occlusion culling (CullingPass Phase 6).
// Each mip level is computed by a separate compute dispatch.
// Owned directly by Renderer (not a RenderGraph pass).

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;

class HiZPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Rebuild the Hi-Z texture when render dimensions change.
    void EnsureTexture(uint32_t depthW, uint32_t depthH);

    // Generate the full mip chain from the GBuffer depth buffer.
    // depthSrvHandle: GPU SRV handle of the depth texture (after GBuffer pass).
    void Execute(RHI::CommandList cl, uint64_t depthSrvHandle);

    // SRV of the Hi-Z mip 0 (full-res min-depth). For occlusion culling.
    uint64_t GetHiZSrvHandle() const;

    const RHI::Texture* GetHiZTexture() const { return &m_hiZTexture; }

private:
    IGraphicsDevice*   m_gfx = nullptr;
    RHI::PipelineState m_pso;          // mip 0 — depth SRV → UAV
    RHI::PipelineState m_reducePSO;    // mip N>0 — UAV → UAV
    ShaderLibrary      m_shaderLib;

    // Hi-Z texture (R32_FLOAT, power-of-2, full mip chain, SRV+UAV)
    RHI::Texture m_hiZTexture;
    uint32_t     m_hiZWidth  = 0;
    uint32_t     m_hiZHeight = 0;
    uint32_t     m_mipCount  = 0;

    struct HiZCB { uint32_t srcW, srcH, dstW, dstH; };

    // CB for per-mip dispatch.
    FrameCB<HiZCB> m_cb;
};
