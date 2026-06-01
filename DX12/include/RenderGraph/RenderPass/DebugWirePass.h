#pragma once

// DebugWirePass — wireframe debug visualization for AABBs and frustum.
// Draws colored line segments on top of the HDR scene.
// Owned by Renderer, not a RenderGraph pass (renders after graph).

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/RenderTypes.h"
#include <DirectXMath.h>
#include <vector>

class IGraphicsDevice;

class DebugWirePass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Add an AABB wireframe (12 edges = 24 vertices).
    void AddAABB(const DirectX::XMFLOAT3& mn, const DirectX::XMFLOAT3& mx, uint32_t color);

    // Add a frustum wireframe (12 edges = 24 vertices) from 8 corner points.
    void AddFrustum(const DirectX::XMFLOAT3 corners[8], uint32_t color);

    // Add a capsule wireframe (two end circles + connecting lines).
    void AddCapsule(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b,
                    float radius, uint32_t color, int segments = 12);

    // Add a sphere wireframe — three great circles in the XY, YZ, and XZ
    // planes. `segments` controls per-circle smoothness; default 16 gives a
    // recognisable sphere silhouette from any viewing angle.
    void AddSphere(const DirectX::XMFLOAT3& center, float radius,
                   uint32_t color, int segments = 16);

    // Add a 3-axis cross marker centred on @p pos with arms of length @p size.
    // Used by the DDGI probe debug visualisation — one cross per probe lets
    // the user see where the volume's grid lands in their scene.
    void AddCross(const DirectX::XMFLOAT3& pos, float size, uint32_t color);

    // Raw line — public so external subsystems (NavMesh wireframe, custom
    // gizmos) can push edges without going through AABB / Frustum helpers.
    void AddLine(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, uint32_t color);

    // Clear all lines (call at start of frame). Also caches this frame's
    // mapped vertex pointer so AddLine's hot path avoids a per-line virtual
    // call (defined in the .cpp — needs the full IGraphicsDevice).
    void Clear();

    bool showCapsules = false;

    // Upload lines to GPU and draw.
    // perViewCB: the PerView constant buffer (contains viewProj).
    void Execute(RHI::CommandList cl, const RHI::Texture* depthTex,
                 const RHI::GPUBuffer& perViewCB);

    bool enabled              = false;
    bool showAABBs            = false;
    bool showFrustum          = false;
    bool showReflectionProbes = false;
    bool showDDGIVolumes      = false;  // when true, AABB + per-probe crosses
    bool  showCollision         = false;  // Mesh ColliderComponent wireframe
    // <= 0 means unlimited range (every Mesh collider in the world). Caller
    // (App.cpp) passes this straight to EmitDebugWireframe; the Debug menu
    // exposes a slider when showCollision is on. Default = no cull because
    // the user explicitly asked for full-scene visibility; toggle the slider
    // for a finite radius when frame time matters.
    float collisionMaxDistance  = 0.0f;

private:
    struct LineVertex
    {
        float    x, y, z;
        uint32_t color; // RGBA8 packed
    };
    static_assert(sizeof(LineVertex) == 16);

    // 2 M vertices = 1 M line segments. Sized for full-scene collision
    // wireframe (Bistro-scale ~1500 entities × ~300 tris × 3 edges easily
    // hits 1 M+ edges) plus navmesh and the usual AABB/frustum overlays.
    // 32 MB upload buffer — well within modern GPU upload-heap budgets.
    // AddLine bails silently if exceeded.
    static constexpr uint32_t kMaxVertices = 2097152;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Vertex buffer is UPLOAD + per-frame written → triple-buffered ring.
    static constexpr uint32_t kFrameCount = 3;
    RHI::GPUBuffer   m_vertexBuffer[kFrameCount];
    void*            m_vertexMapped[kFrameCount] = {};
    uint32_t         m_vertexCount  = 0;

    // Current frame's mapped write target, refreshed by Clear(). Lets AddLine
    // (called once per wireframe edge — millions on a full-scene collision
    // overlay) skip the per-call virtual GetFrameIndex() + null checks.
    LineVertex*      m_curVerts = nullptr;

    PSODesc BuildPSODesc() const;
};
