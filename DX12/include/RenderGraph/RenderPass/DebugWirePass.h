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

    // Add a 3-axis cross marker centred on @p pos with arms of length @p size.
    // Used by the DDGI probe debug visualisation — one cross per probe lets
    // the user see where the volume's grid lands in their scene.
    void AddCross(const DirectX::XMFLOAT3& pos, float size, uint32_t color);

    // Clear all lines (call at start of frame).
    void Clear() { m_vertexCount = 0; }

    bool showCapsules = true;

    // Upload lines to GPU and draw.
    // perViewCB: the PerView constant buffer (contains viewProj).
    void Execute(RHI::CommandList cl, const RHI::Texture* depthTex,
                 const RHI::GPUBuffer& perViewCB);

    bool enabled = false;
    bool showAABBs            = true;
    bool showFrustum          = true;
    bool showReflectionProbes = true;
    bool showDDGIVolumes      = true;  // when true, AABB + per-probe crosses

private:
    struct LineVertex
    {
        float    x, y, z;
        uint32_t color; // RGBA8 packed
    };
    static_assert(sizeof(LineVertex) == 16);

    static constexpr uint32_t kMaxVertices = 65536;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    RHI::GPUBuffer   m_vertexBuffer;
    void*            m_vertexMapped = nullptr;
    uint32_t         m_vertexCount  = 0;

    void AddLine(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, uint32_t color);

    PSODesc BuildPSODesc() const;
};
