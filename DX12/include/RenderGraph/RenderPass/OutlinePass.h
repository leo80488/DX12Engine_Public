#pragma once

// OutlinePass — multi-technique outline rendering for entities with OUTLINE material flag.
//
// Three sub-passes execute sequentially within a single Execute() call:
//
//   1. Inverted Hull (graphics)
//      Renders DrawFilter::Custom packets with front-face culling + clip-space
//      normal extrusion.  Produces a pixel-consistent silhouette outline in HDR.
//
//   2. Object ID (graphics)
//      Renders the same packets into a private R32_UINT texture with standard
//      back-face culling + depth test.  Each pixel receives (meshDescIdx + 1).
//
//   3. Screen-Space Composite (graphics, fullscreen triangle, alpha blend)
//      Reads the Object ID mask, GBuffer normal, and GBuffer depth and runs a
//      Roberts cross edge detector.  Edges are alpha-blended onto the HDR target
//      to draw inner creases and inter-object boundary outlines.
//
// Resource states on entry to Execute():
//   HDR scene  — RENDER_TARGET  (left by TransparentPass)
//   Depth      — DEPTHSTENCIL   (left by TransparentPass WriteDepthStencil)
//   Normal     — SHADER_RESOURCE (left by LightingPass ReadSRV, kept by Setup ReadSRV)
//
// Resource states on exit:
//   HDR scene  — RENDER_TARGET
//   Depth      — DEPTHSTENCIL   (restored after sub-pass 3)
//   Normal     — SHADER_RESOURCE (unchanged)

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"

class OutlinePass : public RG::RenderPass
{
public:
    OutlinePass(RG::RGTextureHandle depth, RG::RGTextureHandle normal);
    ~OutlinePass();

    const char* GetName() const override { return "OutlinePass"; }
    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override
    { return { &m_hullPsoCache, &m_objectIdPsoCache, &m_screenSpacePsoCache }; }

    // ---- Per-frame settings ------------------------------------------------
    float outlinePixels   = 2.0f;
    float outlineColor[3] = { 0.0f, 0.0f, 0.0f };
    float depthThreshold  = 0.05f;   // relative linear depth ratio (was 0.0008 raw)
    float normalThreshold = 0.3f;
    float outlineStrength = 1.0f;
    float outlineFadeStart = 15.0f;  // distance where outline starts fading
    float outlineFadeEnd   = 40.0f;  // distance where outline fully disappears
    float nearZ            = 0.1f;   // set by Renderer each frame
    float farZ             = 1000.f; // set by Renderer each frame

private:
    struct alignas(16) OutlineCBData
    {
        float    outlinePixels    = 2.0f;
        float    depthThreshold   = 0.0008f;
        float    normalThreshold  = 0.3f;
        float    outlineStrength  = 1.0f;
        float    outlineColor[3]  = {};
        float    outlineFadeStart = 15.0f;
        uint32_t vpWidth          = 0;
        uint32_t vpHeight         = 0;
        float    outlineFadeEnd   = 40.0f;
        float    nearZ            = 0.1f;
        float    farZ             = 1000.0f;
        float    _pad0[3]         = {};
    };

    PSODesc BuildHullPSODesc()        const;
    PSODesc BuildObjectIdPSODesc()    const;
    PSODesc BuildScreenSpacePSODesc() const;

    void RebuildObjectIdTexture(IGraphicsDevice& gfx, uint32_t w, uint32_t h);

    // ---- Graph handles -----------------------------------------------------
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_normal;

    // ---- Shaders + PSO caches ----------------------------------------------
    ShaderLibrary m_shaderLib;
    PSOCache      m_hullPsoCache;
    PSOCache      m_objectIdPsoCache;
    PSOCache      m_screenSpacePsoCache;

    // ---- ObjectID private render target (R32_UINT) -------------------------
    // RTV auto-created by RHI::CreateTexture (RENDER_TARGET bind flag).
    RHI::Texture         m_objectIdTex;
    RHI::ResourceState   m_objectIdState = RHI::ResourceState::UNDEFINED;
    uint32_t             m_objectIdW     = 0;
    uint32_t             m_objectIdH     = 0;

    // ---- OutlineCB ---------------------------------------------------------
    RHI::GPUBuffer m_outlineCB;
    void*          m_outlineCBMapped = nullptr;

    IGraphicsDevice* m_gfxPtr = nullptr;
};
