#pragma once

// HeightFogPass — UE-style analytic exponential height fog.
//
// One fullscreen ONE/INV_SRC_ALPHA draw between CloudPass and VideoPass: at
// that point HDR already holds lit geometry (+aerial perspective), skybox,
// water and clouds, so all of them — including the sky at the horizon — get
// fogged; the froxel VolumetricFogPass later composes its near-range detail
// OVER this far fog, and video overlays stay unfogged.
//
// Authoring lives on HeightFogComponent (Sky entity); Renderer_IBL's
// SyncSkyboxIBL pushes it here each frame. Sun dir/colour are read by the
// shader straight from LightCB (b1) — no per-pass sun plumbing.
//
// No root-signature change: LightCB at b1 (BindCBByName 0), a new named CB
// "HeightFogCB" at b2 (BindCBByName 1, registered per frame by
// Render_BindFrameResources), depth via BindSRVByHandle(3) → t5 space0.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "ECS/HeightFogComponent.h"

#include <vector>

class HeightFogPass : public RG::RenderPass
{
public:
    explicit HeightFogPass(RG::RGTextureHandle depth) : m_depth(depth) {}
    ~HeightFogPass();

    const char* GetName() const override { return "HeightFogPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // ---- Renderer-pushed per-frame state ----------------------------------
    void SetEnabled(bool on)             { m_enabled = on; }
    bool IsEnabled() const               { return m_enabled; }
    void SetViewModeHidden(bool h)       { m_viewModeHidden = h; }
    void SetParams(const HeightFogComponent& c) { m_params = c; }

    /** Current-frame slot of the triple-buffered HeightFogConstants CB;
     *  Renderer registers it with the RenderGraph each frame under
     *  "HeightFogCB" (bound to b2 space0). Rebind every frame — the
     *  underlying buffer rotates. */
    const RHI::GPUBuffer& GetFogCB(IGraphicsDevice& gfx) const { return m_fogCB.CurrentBuffer(gfx); }

    // HLSL CB mirror — keep aligned with HeightFogApply.ps.hlsl (3 rows).
    struct alignas(16) HeightFogConstants
    {
        float fogDensity;            // row 0
        float fogHeightFalloff;
        float fogHeight;
        float startDistance;

        float fogColor[3];           // row 1
        float maxOpacity;

        float sunInscatterIntensity; // row 2
        float anisotropy;
        float _pad0;
        float _pad1;
    };
    static_assert(sizeof(HeightFogConstants) == 48, "HeightFogCB layout drift");

private:
    RG::RGTextureHandle m_depth;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    FrameCB<HeightFogConstants> m_fogCB;
    int  m_linearSampler  = -1;
    bool m_enabled        = false;
    bool m_viewModeHidden = false;

    HeightFogComponent m_params;
};
