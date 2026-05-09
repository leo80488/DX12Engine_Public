#pragma once

// UIPass — uploads a UIDrawList and renders it on top of the LDR final-output
// texture (post-tonemap). Owned by Renderer (NOT a RG::RenderPass), runs after
// every other graph pass, before the swap-chain composite. Mirrors the
// DebugWirePass pattern: dedicated CL, dependency-chained to the previous pass,
// transitions the target SR → RT, draws, transitions back to SR.
//
// Resource bindings (engine default root signature):
//   b1 space0 — UICB (canvas pixel size)
//   t2 space0 — vertex ByteAddressBuffer (UPLOAD heap, single-buffered)
//   t3 space0 — current draw command's texture SRV (or 1×1 white)
//   s0        — linear-clamp sampler (created in Init, bound via BindSampler)

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "UI/UIDrawList.h"

class IGraphicsDevice;

class UIPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // The CPU-side draw list. Widgets / UISystem populate this each frame.
    UI::UIDrawList& GetDrawList()             { return m_drawList; }
    const UI::UIDrawList& GetDrawList() const { return m_drawList; }

    // Render @p target — must be RGBA8 UNORM with RENDER_TARGET bind flag.
    // @p entryState is the current tracked state of @p target; UIPass emits a
    // barrier from entryState → RENDERTARGET, draws, then emits RT → SR.
    // On return the target is in SHADER_RESOURCE state (caller updates its
    // own tracker to match). canvasW/canvasH = pixel dimensions of the target
    // (NOT the swap chain — pass the editor viewport size).
    // Caller owns CL spawning + dependency-chaining (mirrors DebugWirePass).
    void Execute(RHI::CommandList cl,
                 const RHI::Texture* target,
                 RHI::ResourceState entryState,
                 uint32_t canvasW, uint32_t canvasH);

    // Hot-reload entry — clears the PSO cache; next Execute rebuilds.
    void ReloadShaders();

    bool enabled = true;

    // Buffer caps (each frame is rebuilt from CPU; oversize the buffer once).
    // 32K verts = 8K quads, 65K indices stays inside uint16 range (Dear ImGui parity).
    static constexpr uint32_t kMaxVertices = 32768;
    static constexpr uint32_t kMaxIndices  = 65535;

private:
    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    UI::UIDrawList   m_drawList;

    // Vertex / index UPLOAD buffers, single-allocated, mapped persistently.
    RHI::GPUBuffer   m_vertexBuffer;
    RHI::GPUBuffer   m_indexBuffer;
    void*            m_vbMapped = nullptr;
    void*            m_ibMapped = nullptr;

    // Per-pass UI CB (canvas size).
    RHI::GPUBuffer   m_cb;
    void*            m_cbMapped = nullptr;

    // 1×1 white default texture (used when a draw cmd has no texture bound).
    RHI::Texture     m_whiteTex;
    uint64_t         m_whiteTexSrv = 0;

    // Linear/clamp sampler (registered once in Init, bound at slot 0).
    int              m_samplerIdx = -1;

    PSODesc BuildPSODesc() const;
    bool    CreateWhiteTexture();
};
