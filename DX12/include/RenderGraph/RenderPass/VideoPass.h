#pragma once

// VideoPass — composite a decoded video frame onto HDR scene color.
//
// Pipeline:
//   1. Renderer scans World for VideoComponents in `Playing` / `Paused`
//      state with a valid currentDpbSlot, picks the first one and calls
//      SetActiveFrame(component) on the pass.
//   2. Renderer also calls backend->AddDecodeDependency(decoder, ...) so the
//      graphics queue waits for the video queue's decode fence before this
//      pass samples the NV12 output.
//   3. Pass binds Y plane SRV → t6 space0, UV plane SRV → t7 space0,
//      writes RGB into HDR via the VideoComposite VS+PS, alpha-blended over
//      whatever the previous passes drew.
//
// Surface mode (set by SetRect):
//   * Fullscreen      — rectUV = (0,0,1,1), default, replaces scene image.
//   * Letterbox       — sub-rect computed from source vs. screen aspect; the
//                       Renderer's helper preserves aspect ratio by default.
//   * Custom overlay  — any rectUV the author wants (in-game TV, debug
//                       preview, picture-in-picture).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <vector>

struct VideoComponent;

class VideoPass : public RG::RenderPass
{
public:
    VideoPass();
    ~VideoPass();

    const char* GetName() const override { return "VideoPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // ---- Renderer-pushed per-frame state ----------------------------------

    // Per-frame: the VideoComponent whose latest decoded frame should be
    // composited this tick. Pass nullptr to skip the pass (Execute becomes a
    // no-op). Renderer is responsible for AddDecodeDependency before Execute.
    void SetActiveFrame(const VideoComponent* vc);

    // Sub-rectangle of HDR that receives the video, in normalised screen
    // space [0,1]. Default = fullscreen. Aspect-preserving letterbox is up to
    // the caller (Renderer typically computes it from source vs. screen ratio).
    void SetRect(float uMin, float vMin, float uMax, float vMax);

    // Alpha applied uniformly to the composited RGB (1.0 = opaque, default).
    void SetAlpha(float alpha) { m_alpha = alpha; }

    // 0 = BT.709 (HD content, default), 1 = BT.601 (legacy SD content).
    void SetColorSpace(uint32_t cs) { m_colorSpace = cs; }

public:
    // HLSL cbuffer layout — keep aligned with VideoComposite.ps.hlsl b2 space0.
    struct alignas(16) VideoCompositeCB
    {
        float    rectUV[4];      // xy = min, zw = max
        float    alpha;
        uint32_t colorSpace;     // 0 = BT.709, 1 = BT.601
        float    _pad[2];
    };

private:
    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Cached PSO desc — built once in Init, used per Execute via PSOCache.
    bool m_psoCreated = false;

    FrameCB<VideoCompositeCB> m_cb;
    int  m_linearSamplerSlot = -1;

    // Per-frame state (set by Renderer; not owned).
    const VideoComponent* m_activeFrame = nullptr;
    float    m_rectUV[4]  = { 0.f, 0.f, 1.f, 1.f };
    float    m_alpha      = 1.f;
    uint32_t m_colorSpace = 0;
};
