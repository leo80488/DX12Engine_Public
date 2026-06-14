#pragma once

// WaterPass — forward water surface: Fresnel sky reflection + flow normals.
//
// Runs right after SkyboxPass (sky pixels behind the water already exist in
// HDR) and before CloudPass / VolumetricFogPass, so clouds composite above
// the water and fog correctly attenuates it. Draws a procedural grid
// (SV_VertexID, no buffers) into the HDR target with the GBuffer depth
// bound: depth test GREATER_EQUAL (reversed-Z) + depth WRITE so later
// depth-aware passes (clouds, fog) treat the surface as real geometry.
// Alpha blend gives the analytic shoreline fade.
//
// Water depth (absorption tint + shore fade) is computed in the PS from the
// terrain heightmap — water plane Y minus terrain Y — because the engine has
// no read-only DSV (cannot hardware-depth-test and sample scene depth in the
// same draw). The same heightmap UV math as Terrain.ms.hlsl is used.
//
// Reflection sources:
//   - sky cube: SkyIBLPass::ResolveSkyboxSrvHandle() (atmosphere cube or the
//     static skybox .itex) — pushed per frame by the Renderer.
//   - SSR: the water joins the engine's deferred SSR pipeline. It WRITES its
//     per-pixel flow normal into GBuffer1 and (roughness, 0, 1, 0.5) into
//     GBuffer2 (depth was already written), so the post-frame SSR chain
//     traces wavy reflections off the water surface and SSRComposite adds
//     ssr×F×conf onto the HDR the SAME frame. The water PS reads the
//     PREVIOUS frame's SSR result (t27, same 1-frame-latent convention as
//     Lighting.ps) only to dampen its sky-reflection term by (1−conf) so the
//     composite's addition doesn't double-count — sampled at the surface
//     point's prev-frame UV (via prevClip) so the stale mask doesn't shear
//     off the current reflection footprint while the camera moves.
//
// Renderer wiring (Renderer_GrassWater.cpp / BuildScene_SyncWater): fills
// WaterCBData from the first WaterComponent (+ first TerrainComponent for
// the depth mapping), publishes the CB as "WaterParams", arms the pass via
// SetActiveWater(). Pass self-skips when there is no water entity or the
// sky cube is not ready yet.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

namespace RG { class RenderContext; }

// HLSL-layout mirror of the WaterCB in Water.vs/ps.hlsl (b2 space0). 9 rows.
struct alignas(16) WaterCBData
{
    // row 0
    float    waterOriginX, waterOriginZ;
    float    waterSize;
    float    waterHeight;
    // rows 1/2
    float    deepColor[4];
    float    shallowColor[4];
    // row 3
    float    flowDirX, flowDirZ;
    float    flowSpeed;
    float    normalTiling;
    // row 4
    float    normalStrength;
    float    absorbDist;
    float    shoreFade;
    float    fresnelF0;
    // row 5
    float    specPower;
    float    reflStrength;
    float    time;
    uint32_t gridQuads;
    // row 6
    float    terrainOriginX, terrainOriginZ;
    float    terrainSize;
    float    terrainBaseY;
    // row 7
    float    terrainHeightScale;
    uint32_t hasTerrain;
    float    hmTexel;
    float    surfaceRoughness;   // GBuffer2.r at water pixels (drives SSR cone)
    // row 8
    float    hmUVOffsetX, hmUVOffsetY;
    float    hmUVScaleX,  hmUVScaleY;
};
static_assert(sizeof(WaterCBData) == 144,
    "WaterCBData layout drift — sync Water.vs.hlsl / Water.ps.hlsl WaterCB");

class WaterPass : public RG::RenderPass
{
public:
    WaterPass(RG::RGTextureHandle normal,
              RG::RGTextureHandle surface,
              RG::RGTextureHandle velocity,
              RG::RGTextureHandle depth);

    const char* GetName() const override { return "WaterPass"; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Per-frame state pushed by Renderer before the graph executes.
    // gridQuads inside the CB drives the draw's vertex count; active == false
    // → skip the draw this frame.
    struct WaterBindings
    {
        uint64_t heightmapSRV = 0;   // 0 → pass-owned 1×1 fallback (no terrain)
        uint64_t normalASRV   = 0;   // flow-normal map A (t4); 0 → flat fallback
        uint64_t normalBSRV   = 0;   // flow-normal map B (t5); 0 → flat fallback
        bool     active       = false;
    };
    void SetActiveWater(const WaterBindings& b, const WaterCBData& cb)
    {
        m_water     = b;
        m_pendingCB = cb;
    }

    // Reflection source — pushed by Renderer::SyncSkyboxIBL right where the
    // SkyboxPass env map is resolved, so the water always reflects exactly
    // the sky being drawn. 0 → skip the draw.
    void SetSkyCube(uint64_t srv) { m_skyCubeSRV = srv; }

    // Previous-frame SSR result (rgb = reflection, a = confidence) — pushed
    // by Renderer from the same place LightingPass gets it. 0 → pass-owned
    // 1×1 black fallback (conf 0 → pure sky reflection, no dampening).
    void SetSSRResult(uint64_t srv) { m_ssrResultSRV = srv; }

    // CSM cascade array (same handle LightingPass binds) — the sun glint is
    // direct light and must respect terrain shadows. Pushed per frame by
    // Renderer::Render_BindFrameResources; 0 → bind skipped (shader's
    // shadowStrength==0 early-out keeps the unbound table unreferenced).
    void SetShadowMap(uint64_t srv) { m_shadowSrvHandle = srv; }

    // Global view-mode suppress — skip in Wireframe view (same convention as
    // SkyboxPass / CloudPass).
    void SetViewModeHidden(bool h) { m_viewModeHidden = h; }

    // Current-frame slot of the triple-buffered WaterCB; Renderer registers
    // it with the RenderGraph each frame under the name "WaterParams".
    const RHI::GPUBuffer& GetWaterCB(IGraphicsDevice& gfx) const
    {
        return m_waterCB.CurrentBuffer(gfx);
    }

private:
    PSODesc BuildPSODesc() const;

    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;
    int                 m_clampSamplerIdx  = -1;  // s0 — heightmap + sky cube
    int                 m_wrapSamplerIdx   = -1;  // s1 — flow-normal maps (world-XZ tiling)
    int                 m_shadowSamplerIdx = -1;  // s2 — CSM PCF comparison (GREATER_EQUAL)
    RG::RGTextureHandle m_normal;    // GBuffer1 — water flow normal for SSR trace
    RG::RGTextureHandle m_surface;   // GBuffer2 — water roughness for SSR cone/Fresnel
    RG::RGTextureHandle m_velocity;  // GBuffer3 — water motion vectors (TAA/SSR temporal)
    RG::RGTextureHandle m_depth;

    FrameCB<WaterCBData> m_waterCB;
    WaterCBData          m_pendingCB{};
    WaterBindings        m_water{};
    uint64_t             m_skyCubeSRV      = 0;
    uint64_t             m_ssrResultSRV    = 0;
    uint64_t             m_shadowSrvHandle = 0;
    bool                 m_viewModeHidden  = false;

    // 1×1 R16_UNORM fallback so root slot 10 always holds a valid Texture2D
    // descriptor when no terrain exists.
    RHI::Texture m_fallbackHeightmap;
    uint64_t     m_fallbackHeightmapSRV = 0;

    // 1×1 flat tangent-space normal (128,128,255) for t4/t5 while the flow
    // maps are still loading / unset — calm water instead of garbage.
    RHI::Texture m_fallbackFlatNormal;
    uint64_t     m_fallbackFlatNormalSRV = 0;

    // 1×1 black for t27 when SSR is disabled / not yet produced — conf reads
    // 0 so the shader's (1−conf) dampening is a no-op.
    RHI::Texture m_fallbackBlack;
    uint64_t     m_fallbackBlackSRV = 0;
};
