#pragma once

// SSRUpsamplePass — Pass 5 of the Hi-Z SSR pipeline. RESOLUTION BOUNDARY.
//
// Reads half-res temporal color + variance, runs a variance-driven bilateral
// blur, and writes a FULL-resolution reflection texture. LightingPass binds
// the output as a 1-frame-latent SRV for (1 - ssrConf) IBL dampening;
// SSRComposite additively blends it into the HDR pass.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRUpsamplePass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);
    // Output stays at RENDER-resolution — this pass upsamples half-res
    // trace/resolve/temporal back to full-res via bilateral sampling.
    void EnsureTexture(uint32_t renderW, uint32_t renderH);

    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;
        float               nearZ;
        float               farZ;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }

    uint64_t GetColorSrv() const;
    const RHI::Texture* GetColorTexture() const { return &m_colorTex; }

    void Execute(RHI::CommandList cl,
                 uint32_t w, uint32_t h,
                 uint64_t temporalSrv, uint64_t varianceSrv,
                 uint64_t depthSrv, uint64_t normalSrv, uint64_t surfaceSrv);

public:
    struct alignas(16) SSRUpsampleCB
    {
        float    invViewProj[16];
        uint32_t screenW;     uint32_t screenH;
        float    invScreenW;  float    invScreenH;
        float    nearZ;       float    farZ;
        float    _pad0;       float    _pad1;
    };

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<SSRUpsampleCB> m_cb;

    RHI::Texture m_colorTex;       // RGBA16F (SR+UAV) — full render res
    uint32_t m_w = 0, m_h = 0;
    Camera   m_cam{};
};
