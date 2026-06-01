#pragma once

// CullingPass — GPU compute frustum culling.
//
// Dispatches InstanceCull.cs.hlsl: reads GPUInstanceData[], tests AABB vs frustum,
// outputs compacted IndirectDrawCommand[] and a draw count.
// Owned directly by Renderer (not a RenderGraph pass).

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"
#include <DirectXMath.h>

class IGraphicsDevice;

class CullingPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Set per-frame data before Execute.
    void SetFrustumPlanes(const float frustumPlanes[6][4]); // 6 planes, each {nx,ny,nz,d}
    void SetViewProj(const DirectX::XMFLOAT4X4& vp) { m_viewProj = vp; }
    void SetInstanceCount(uint32_t count) { m_instanceCount = count; }

    // Dispatch the culling compute shader.
    // instanceBuffer: SRV of GPUInstanceData[]
    // meshAABBBuffer: SRV of MeshAABB[]
    // outArgBuffer:   UAV of IndirectDrawCommand[] (output)
    // outCountBuffer: UAV of uint32 (output draw count, atomic)
    void Execute(RHI::CommandList cl,
                 const RHI::GPUBuffer& instanceBuffer,
                 const RHI::GPUBuffer& meshAABBBuffer,
                 const RHI::GPUBuffer& outArgBuffer,
                 const RHI::GPUBuffer& outCountBuffer);

    uint32_t GetOutputDrawCount() const { return m_instanceCount; } // max (actual count is GPU-side)

private:
    IGraphicsDevice*    m_gfx = nullptr;
    RHI::PipelineState  m_pso;
    ShaderLibrary       m_shaderLib;

    // CB data
    struct alignas(16) CullingCB
    {
        float    frustumPlanes[6][4]; // 96 bytes
        DirectX::XMFLOAT4X4 viewProj; // 64 bytes
        uint32_t instanceCount;        // 4 bytes
        uint32_t _pad[3];             // 12 bytes
    };                                 // total: 176 bytes
    static_assert(sizeof(CullingCB) % 16 == 0);

    CullingCB           m_cbData{};
    FrameCB<CullingCB>  m_cb;

    DirectX::XMFLOAT4X4 m_viewProj{};
    uint32_t m_instanceCount = 0;
};
