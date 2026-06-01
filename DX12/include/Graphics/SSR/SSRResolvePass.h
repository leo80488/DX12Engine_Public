#pragma once

// SSRResolvePass — Pass 3 of the Hi-Z SSR pipeline.
//
// Spatial BRDF reweight: reads trace's per-pixel hits, reuses 4 neighbours
// in HALF-RES space, and writes a BRDF-weighted reflection color + Welford
// variance estimate + reprojection depth. Also owns the HDR snapshot copy
// that feeds the scene-color pyramid.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRResolvePass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);
    // Render-resolution dims; internal textures sized at full render res.
    // When aliasHeap != nullptr the snapshot + color textures are created as
    // PLACED resources at the given heap offsets (transient aliasing); variance
    // + reprojDepth stay committed. nullptr (default) → all committed.
    void EnsureTextures(uint32_t renderW, uint32_t renderH,
                        ID3D12Heap* aliasHeap = nullptr,
                        uint64_t offSnapshot = 0, uint64_t offColor = 0);


    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3   cameraPos;
        float               nearZ;
        float               farZ;
    };
    void SetCamera(const Camera& cam) { m_cam = cam; }
    void SetFrameIndex(uint32_t f)    { m_frameIndex = f; }
    // Post-accumulation luminance cap. <=0 disables. UE-equivalent ~16.
    void SetFireflyCap(float cap)     { m_fireflyCap = cap; }
    float GetFireflyCap() const       { return m_fireflyCap; }
    uint32_t GetRenderWidth()  const  { return m_renderW; }
    uint32_t GetRenderHeight() const  { return m_renderH; }

    const RHI::Texture* GetSnapshotTexture() const { return &m_snapshotTex; }
    uint64_t            GetSnapshotSrv()     const;
    void PreSnapshotCopy(RHI::CommandList cl);
    void PostSnapshotCopy(RHI::CommandList cl);
    // NOTE: when snapshot/color are placed-aliased (SSRSubsystem scratch-alias
    // path), do NOT reset their tracked state across the aliasing barrier — the
    // debug layer tracks resource state through aliasing barriers, so the normal
    // Pre/PostSnapshotCopy + resolve toUAV/toSR transitions are exactly what it
    // expects. Content is re-initialized by the full copy / full-coverage UAV
    // write (the aliasing init contract). Verified clean with the debug layer.

    const RHI::Texture* GetColorTexture()       const { return &m_colorTex; }
    const RHI::Texture* GetVarianceTexture()    const { return &m_varianceTex; }
    const RHI::Texture* GetReprojDepthTexture() const { return &m_reprojDepthTex; }
    uint64_t GetColorSrv()       const;
    uint64_t GetVarianceSrv()    const;
    uint64_t GetReprojDepthSrv() const;

    void Execute(RHI::CommandList cl,
                 uint32_t traceW, uint32_t traceH,
                 uint64_t normalSrv, uint64_t surfaceSrv, uint64_t depthSrv,
                 uint64_t hitBufferSrv, uint64_t rayDirPDFSrv,
                 uint64_t rayLengthSrv);

    uint32_t GetWidth()  const { return m_w; }
    uint32_t GetHeight() const { return m_h; }

public:
    struct alignas(16) SSRResolveCB
    {
        float    invViewProj[16];
        float    cameraPos[3];   float    _pad0;
        uint32_t traceW;         uint32_t traceH;
        float    invTraceW;      float    invTraceH;
        float    nearZ;          float    farZ;
        uint32_t frameIndex;     uint32_t renderW;
        float    fireflyCap;     uint32_t renderH;
    };

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<SSRResolveCB> m_cb;

    RHI::Texture       m_snapshotTex;           // full-res
    RHI::ResourceState m_snapshotState = RHI::ResourceState::COPY_DST;

    RHI::Texture       m_colorTex;              // RGBA16F — half-res
    RHI::Texture       m_varianceTex;           // R16F    — half-res
    RHI::Texture       m_reprojDepthTex;        // R16F    — half-res
    RHI::ResourceState m_colorState       = RHI::ResourceState::SHADER_RESOURCE;
    RHI::ResourceState m_varianceState    = RHI::ResourceState::SHADER_RESOURCE;
    RHI::ResourceState m_reprojDepthState = RHI::ResourceState::SHADER_RESOURCE;

    uint32_t m_w = 0, m_h = 0;
    uint32_t m_renderW = 0, m_renderH = 0;
    Camera   m_cam{};
    uint32_t m_frameIndex = 0;
    float    m_fireflyCap = 16.0f;
};
