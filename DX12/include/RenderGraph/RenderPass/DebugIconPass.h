#pragma once

// DebugIconPass — editor-only billboard ICONS (light / camera / audio gizmos)
// drawn as a dedicated debug overlay AFTER tonemap, fully separate from the
// gameplay scene draw (GBuffer / Transparent) AND from the gameplay WorldUI
// pass. This is the "Billboards" GPU bucket from editor_debug_draw_system.md —
// the icons no longer ride the gameplay DrawCandidate path.
//
// Reuse, don't reinvent (per the design doc): this pass borrows the WorldUI
// billboard shaders (WorldUI.vs/ps.hlsl) and the same 28-byte vertex layout +
// engine-shared root slots (vertex SRV at t2 space0, bindless texture table at
// t0 space2). It differs only in its data source: an immediate AddIcon() queue
// the editor's DebugDrawSystem fills each frame, instead of ECS components.
// Nothing is submitted in Game builds (DebugDrawSystem is editor-gated), so the
// queue stays empty and Execute() early-outs.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/FrameCB.h"

#include <DirectXMath.h>
#include <vector>
#include <cstdint>

class IGraphicsDevice;

class DebugIconPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders();

    // Per-frame immediate API. DebugDrawSystem clears then fills this each
    // editor frame (after Renderer::BeginFrame, before Renderer::Render).
    void ClearIcons() { m_icons.clear(); }

    // worldPos  — icon centre in world space.
    // halfSize  — world-space half-extent of the camera-facing quad.
    // texIdx    — bindless index into the engine t0 space2 table. A texture's
    //             bindless index == its handle_id (see Renderer_Scene.cpp).
    // color     — 0xAABBGGRR multiply tint (use 0xFFFFFFFF for unmodified).
    void AddIcon(const DirectX::XMFLOAT3& worldPos, float halfSize,
                 uint32_t texIdx, uint32_t color = 0xFFFFFFFFu);

    bool HasIcons() const { return !m_icons.empty(); }

    // Draws the queued icons into @p target (RGBA8 RENDER_TARGET). Mirrors
    // WorldUIBillboardPass::Execute: transitions target SR→RT, draws, RT→SR.
    void Execute(RHI::CommandList            cl,
                 const DirectX::XMFLOAT4X4&  viewProjMatrix,
                 const DirectX::XMFLOAT4X4&  viewMatrix,
                 const RHI::Texture*         target,
                 RHI::ResourceState          entryState,
                 uint32_t                    canvasW,
                 uint32_t                    canvasH);

    bool enabled = true;

    // 6 verts/icon → ~1365 icons. AddIcon past this is dropped silently.
    static constexpr uint32_t kMaxVertices = 8192;

private:
    // Must match the WorldUI vertex layout the shared shaders read.
    struct IconVertex
    {
        float    pos[3];
        float    uv[2];
        uint32_t col;
        uint32_t texIdx;
    };
    static_assert(sizeof(IconVertex) == 28, "IconVertex must match WorldUIVertex (28 bytes)");

    struct IconReq
    {
        DirectX::XMFLOAT3 pos;
        float             halfSize;
        uint32_t          texIdx;
        uint32_t          col;
    };

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    static constexpr uint32_t kFrameCount = 3;          // == GraphicsDX12::FrameCount
    RHI::GPUBuffer   m_vertexBuffer[kFrameCount];
    void*            m_vbMapped[kFrameCount] = {};

    FrameCB<DirectX::XMFLOAT4X4> m_cb;                  // un-jittered viewProj (b1)
    int                          m_samplerIdx = -1;

    std::vector<IconReq> m_icons;

    PSODesc BuildPSODesc() const;
};
