#pragma once

// WorldUIBillboardPass — renders 3D billboarded UI elements (HP bars,
// nameplates, damage numbers, world markers) on top of the tonemapped
// scene.
//
// Reads:
//   * World ECS — entities with WorldSpaceUIComponent + content
//     (WorldUIBar / WorldUIText / WorldUIImage / DamageNumber)
//   * Engine PerViewCB (b1 space0) — viewProj matrix
//   * Camera right / up vectors (computed on CPU each frame from view)
//
// Writes:
//   * Tonemap final-output texture (transitions SR → RT → SR)
//
// Owned by Renderer (NOT a graph pass), invoked after ToneMap and before
// UIPass — world UI sits above 3D scene but below screen-space HUD.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/FrameCB.h"

#include <DirectXMath.h>

class IGraphicsDevice;
class World;
namespace UI { class Font; }

class WorldUIBillboardPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Render every world-space UI entity into @p target.
    // @p target must be RGBA8_UNORM with RENDER_TARGET bind-flag.
    // @p entryState is the texture's current tracked state (caller updates
    // its own tracker on return; pass leaves the texture in SR).
    void Execute(RHI::CommandList            cl,
                 World&                      world,
                 const DirectX::XMFLOAT4X4&  viewProjMatrix,
                 const DirectX::XMFLOAT4X4&  viewMatrix,
                 const RHI::Texture*         target,
                 RHI::ResourceState          entryState,
                 uint32_t                    canvasW,
                 uint32_t                    canvasH);

    void ReloadShaders();

    bool enabled = true;

    static constexpr uint32_t kMaxVertices = 65536;

private:
    // 28-byte vertex — pos(12) + uv(8) + col(4) + texIdx(4).  texIdx is a
    // bindless index into the engine's t0 space2 table; 0xFFFFFFFF skips
    // the texture sample (bar / border quads use this sentinel).
    struct WorldUIVertex
    {
        float    pos[3];
        float    uv[2];
        uint32_t col;
        uint32_t texIdx;
    };
    static_assert(sizeof(WorldUIVertex) == 28, "WorldUIVertex must be 28 bytes");

    static constexpr uint32_t kInvalidTexIdx = 0xFFFFFFFFu;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Per-frame UPLOAD-heap vertex buffer (triple-buffered ring).
    // 3 == GraphicsDX12::FrameCount.
    static constexpr uint32_t kFrameCount = 3;
    RHI::GPUBuffer   m_vertexBuffer[kFrameCount];
    void*            m_vbMapped[kFrameCount] = {};

    // Stub CB at b1 (the engine's PerViewCB lives there in shared root sig;
    // we still upload our own un-jittered viewProj into this slot to avoid
    // TAA jitter visibly shifting world UI between frames).
    FrameCB<DirectX::XMFLOAT4X4> m_cb;

    int              m_samplerIdx = -1;

    PSODesc BuildPSODesc() const;

    // Push a billboarded quad with a bindless texIdx (kInvalidTexIdx for
    // bar/border verts so the PS short-circuits to vertex colour).
    static void EmitQuad(std::vector<WorldUIVertex>& out,
                         const DirectX::XMFLOAT3& worldCenter,
                         const DirectX::XMFLOAT3& cameraRightWS,
                         const DirectX::XMFLOAT3& cameraUpWS,
                         float halfW, float halfH,
                         float u0, float v0, float u1, float v1,
                         uint32_t col, uint32_t texIdx);
};
