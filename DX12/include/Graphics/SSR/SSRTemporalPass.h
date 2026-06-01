#pragma once

// SSRTemporalPass — Pass 4 of the Hi-Z SSR pipeline.
//
// Dual-reprojection history accumulation: blends resolve output with prev-
// frame history using both velocity-based reprojection (surface motion) and
// reflection-hit reprojection (the hit point's prev-frame UV). Maintains
// ping-pong half-res color/variance buffers + half-res depth history for
// disocclusion detection.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRTemporalPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);
    void EnsureTextures(uint32_t renderW, uint32_t renderH);

    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;   // current inverse, jittered
        DirectX::XMFLOAT4X4 prevViewProj;  // previous-frame VP, jittered
        float               nearZ;
        float               farZ;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }
    void SetFrameIndex(uint32_t f)    { m_frameIndex = f; }
    void MarkReset()                  { m_resetHistory = true; }

    uint64_t GetColorSrv()    const;
    uint64_t GetVarianceSrv() const;
    const RHI::Texture* GetColorTexture()    const;
    const RHI::Texture* GetVarianceTexture() const;

    void Execute(RHI::CommandList cl,
                 uint32_t traceW, uint32_t traceH,
                 uint64_t colorCurrentSrv,
                 uint64_t varianceCurrentSrv,
                 uint64_t reprojDepthSrv,
                 uint64_t velocitySrv,
                 uint64_t depthSrv);

    uint32_t GetRenderWidth()  const { return m_renderW; }
    uint32_t GetRenderHeight() const { return m_renderH; }

public:
    struct alignas(16) SSRTemporalCB
    {
        float    invViewProj[16];   // jittered (matches depth rasterization)
        float    prevViewProj[16];  // prev-frame jittered (matches history grid)
        uint32_t traceW;      uint32_t traceH;
        float    invTraceW;   float    invTraceH;
        uint32_t resetHistory;
        float    nearZ;       float    farZ;
        uint32_t renderW;     uint32_t renderH;
        uint32_t frameIndex;  uint32_t _pad[1];
    };

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<SSRTemporalCB> m_cb;

    RHI::Texture m_color[2];        // RGBA16F ping-pong (half-res)
    RHI::Texture m_variance[2];     // R16F ping-pong (half-res)
    RHI::Texture m_depthHistory[2]; // R32F ping-pong (half-res, reverse-Z NDC)

    uint32_t m_w = 0, m_h = 0;
    uint32_t m_renderW = 0, m_renderH = 0;
    uint32_t m_writeIdx = 0;
    uint32_t m_frameIndex = 0;
    bool     m_resetHistory = true;
    Camera   m_cam{};
};
