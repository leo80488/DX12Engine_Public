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
#include "Graphics/FrameCB.h"
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

    // Vertex UPLOAD buffer (triple-buffered ring — 3 == GraphicsDX12::FrameCount).
    // The index buffer member below is currently unused — UIPass flat-expands
    // indices on CPU into the VB at upload time (engine root sig has no IA
    // index-buffer slot). Kept declared so a future engine-side IA index path
    // can wire it in without ABI churn.
    static constexpr uint32_t kFrameCount = 3;
    RHI::GPUBuffer   m_vertexBuffer[kFrameCount];
    void*            m_vbMapped[kFrameCount] = {};
    RHI::GPUBuffer   m_indexBuffer;
    void*            m_ibMapped = nullptr;

    // Per-pass UI CB (canvas size).
    struct alignas(16) UICB { float w, h; uint32_t pad0, pad1; };
    FrameCB<UICB>    m_cb;

    // SDF text effects table — uploaded from UIDrawList::Effects() each frame
    // and bound at b2 space0. Indexed by the draw command's effectIndex (which
    // the PS reads from b0 root constants). Element 0 is the no-op effect.
    static constexpr uint32_t kMaxEffectSlots = 64;
    struct alignas(16) UIEffectsTable { UI::GpuTextEffect fx[kMaxEffectSlots]; };
    FrameCB<UIEffectsTable> m_effectsCB;

    // 1×1 white default texture (used when a draw cmd has no texture bound).
    RHI::Texture     m_whiteTex;
    uint64_t         m_whiteTexSrv = 0;

    // UV samplers, indexed by UI::UISamplerId(wrap, pointFilter):
    //   [0..2] linear Clamp/Wrap/Mirror, [3..5] point Clamp/Wrap/Mirror.
    // Bound per draw command at s0. SDF text always uses [0] (clamp-linear).
    int              m_samplers[UI::kUISamplerCount] = { -1, -1, -1, -1, -1, -1 };

    PSODesc BuildPSODesc() const;
    bool    CreateWhiteTexture();
};
