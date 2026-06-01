#pragma once

// VideoQuadPass — render VideoComponents that opt into world-space (a
// `worldWidth × worldHeight` quad placed by the entity's GlobalTransform).
//
// Sits in the graph AFTER the opaque + transparent passes so the quad
// participates in proper depth ordering (occluded by closer opaque
// geometry, blends over transparent + skybox). Depth test ON, depth
// write OFF — same convention TransparentPass uses for layered content.
//
// Per-entity flow (Execute):
//   1. world.ForEach<VideoComponent> — skip when worldSpace=false / not
//      Playing-or-Paused / no decoded frame yet.
//   2. Resolve the entity's GlobalTransform.matrix (identity fallback).
//   3. Upload VideoQuadCB { worldMat, viewProj, w, h, alpha, colorSpace }
//      into a per-component slot of FrameCB ring.
//   4. Bind Y/UV plane SRVs (t6 / t7 space0) of the active DPB texture and
//      DrawInstanced(6, 1) — procedural quad in the VS.
//
// Cross-queue sync with the video decode queue is inserted by Renderer
// (one AddDecodeDependency per active decoder) before the graph submits,
// so the PS sampling the NV12 output always sees retired data.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"

#include <DirectXMath.h>
#include <vector>

class VideoQuadPass : public RG::RenderPass
{
public:
    explicit VideoQuadPass(RG::RGTextureHandle depth);
    ~VideoQuadPass();

    const char* GetName() const override { return "VideoQuadPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Renderer pushes the (un-jittered) scene viewProj once per frame before
    // Execute. The pass cannot reach into Renderer for it because no other
    // RenderPass member-includes Renderer.h — keep the data flowing through
    // explicit setters, matching CloudPass / VolumetricFogPass.
    void SetCamera(const DirectX::XMFLOAT4X4& viewProjNoJitter);

    // Pull the World pointer from Renderer per-frame so Execute can iterate.
    // Pointer non-owned, must remain valid until Execute returns.
    void SetWorld(class World* world) { m_world = world; }

public:
    // CB layout — must match VideoQuad.vs.hlsl + VideoQuad.ps.hlsl b2 space0.
    struct alignas(16) VideoQuadCB
    {
        float    worldMatrix [16];  // 64 B
        float    viewProjMatrix[16]; // 64 B
        float    quadWidth;
        float    quadHeight;
        float    alpha;
        uint32_t colorSpace;          // → 16 B trailing block
    };

private:
    RG::RGTextureHandle m_depth;

    IGraphicsDevice* m_gfx   = nullptr;
    class World*     m_world = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;
    bool             m_psoCreated = false;

    // Multi-slot CB ring: one UPLOAD-heap buffer per backbuffer slot,
    // persistently mapped, sub-allocated per draw at 256 B (D3D12 CBV
    // alignment) intervals. Auto-grows when a frame asks for more slots
    // than the current capacity provides.
    static constexpr uint32_t kCBStride       = 256;  // D3D12 CB alignment
    static constexpr uint32_t kCBInitialSlots = 16;
    static constexpr uint32_t kFrameCount     = 3;

    struct CBRing
    {
        RHI::GPUBuffer buffer;
        uint8_t*       mapped = nullptr;
        uint32_t       capacitySlots = 0;
    };
    CBRing   m_cbRing[kFrameCount];
    uint32_t m_pendingGrowSlots = 0;       // grow next frame when > 0

    int      m_linearSamplerSlot = -1;
    DirectX::XMFLOAT4X4 m_viewProj{};

    // (re-)allocate the ring buffer for one frame slot. Frees the existing
    // buffer via the standard deferred-release path.
    void EnsureCBRing(IGraphicsDevice& gfx, uint32_t frameIdx, uint32_t slots);
};
