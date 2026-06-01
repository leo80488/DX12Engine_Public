#pragma once

// SSRCompositePass — Pass 6 of the Hi-Z SSR pipeline.
//
// Additive blend of resolved SSR into the HDR scene color, weighted by
// Fresnel × envBRDF × confidence × intensity. Pairs with LightingPass's
// own (1 - ssrConf) IBL dampening so the combined specular term is
// energy-correct.
//
// Also hosts debug-visualisation modes — every non-zero CB.debugMode
// fully overwrites HDR with a diagnostic view of an upstream stage so the
// editor's SSR debug window can pinpoint which stage broke.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRCompositePass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);

    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3   cameraPos;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }

    void  SetIntensity(float i)   { m_intensity = i; }
    float GetIntensity() const    { return m_intensity; }
    void SetDebugMode(uint32_t m) { m_debugMode = m; }
    uint32_t GetDebugMode() const { return m_debugMode; }

    void Execute(RHI::CommandList cl,
                 uint32_t w, uint32_t h,
                 uint64_t albedoSrv, uint64_t normalSrv, uint64_t surfaceSrv,
                 uint64_t depthSrv,  uint64_t ssrSrv,    uint64_t hdrUav,
                 uint64_t brdfLutSrv,
                 uint64_t rawTraceSrv = 0, uint64_t rayDirSrv = 0,
                 uint64_t rayLenSrv   = 0, uint64_t varianceSrv = 0);

    void SetRoughnessCutoff(float v) { m_roughnessCutoff = v; }

public:
    struct alignas(16) SSRCompositeCB
    {
        float    invViewProj[16];
        float    cameraPos[3];   float    _pad0;
        uint32_t screenW;        uint32_t screenH;
        float    intensity;      uint32_t debugMode;
        float    roughnessCutoff; float   _pad1;
    };

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<SSRCompositeCB> m_cb;

    Camera   m_cam{};
    float    m_intensity       = 1.0f;
    uint32_t m_debugMode       = 0;
    float    m_roughnessCutoff = 0.5f;
};
