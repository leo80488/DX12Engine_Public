#pragma once

// OutlinePass — multi-technique outline rendering for entities with OUTLINE material flag.
//
// Pre-TAA composite: registered in the main render graph after TransparentPass.
// Outline pixels are written into HDR scene color at the same JITTERED NDC as
// the main GBuffer raster, so TAA reprojects+history-blends mesh and outline
// together — outline never desyncs from the mesh during animation. The
// trade-off is that outline edges share TAA's tent-filter softening with the
// mesh (~0.5 px), but the softening is symmetric so it's not perceptible as a
// gap. This is the same architecture Honkai / Genshin-style toon games use.
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
#include "Graphics/FrameCB.h"

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
    { return { &m_hullPsoCache, &m_objectIdPsoCache,
               &m_pickingObjectIdPsoCache, &m_screenSpacePsoCache }; }

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

    // Editor selection outline color. Drawn by sub-pass 3 only for pixels whose
    // ObjectID has bit-31 set; sub-pass 2 sets that bit for picking-outline draws
    // (rendered with depth_enable=false → silhouette visible through walls).
    float pickingOutlineColor[3] = { 0.95f, 0.80f, 0.10f }; // Blender-ish yellow

    // TEMP DIAG: runtime toggles to isolate which sub-pass causes the perceived
    // "outline lag" during character motion.  Flip in the editor inspector or
    // via code and observe whether the artifact persists.
    bool diagSkipHull        = false;   // set true → skip sub-pass 1 entirely
    bool diagSkipScreenSpace = false;   // set true → skip sub-pass 3 entirely

private:
    struct alignas(16) OutlineCBData
    {
        float    outlinePixels       = 2.0f;
        float    depthThreshold      = 0.0008f;
        float    normalThreshold     = 0.3f;
        float    outlineStrength     = 1.0f;
        float    outlineColor[3]     = {};
        float    outlineFadeStart    = 15.0f;
        uint32_t vpWidth             = 0;
        uint32_t vpHeight            = 0;
        float    outlineFadeEnd      = 40.0f;
        float    nearZ               = 0.1f;
        float    farZ                = 1000.0f;
        // Picking-outline color (replaces former _pad0). Used by sub-pass 3 when
        // ObjectID has bit-31 set (written by picking ObjectID PSO).
        float    pickingOutlineColor[3] = {};
    };

    PSODesc BuildHullPSODesc()             const;
    PSODesc BuildObjectIdPSODesc()         const;
    PSODesc BuildPickingObjectIdPSODesc()  const; // depth-test off → silhouette through walls
    PSODesc BuildScreenSpacePSODesc()      const;

    void RebuildObjectIdTexture(IGraphicsDevice& gfx, uint32_t w, uint32_t h);

    // ---- Graph handles -----------------------------------------------------
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_normal;

    // ---- Shaders + PSO caches ----------------------------------------------
    ShaderLibrary m_shaderLib;
    PSOCache      m_hullPsoCache;
    PSOCache      m_objectIdPsoCache;
    PSOCache      m_pickingObjectIdPsoCache; // depth-test off (x-ray silhouette)
    PSOCache      m_screenSpacePsoCache;

    // ---- ObjectID private render target (R32_UINT) -------------------------
    // RTV auto-created by RHI::CreateTexture (RENDER_TARGET bind flag).
    RHI::Texture         m_objectIdTex;
    RHI::ResourceState   m_objectIdState = RHI::ResourceState::UNDEFINED;
    uint32_t             m_objectIdW     = 0;
    uint32_t             m_objectIdH     = 0;

    // ---- OutlineCB ---------------------------------------------------------
    FrameCB<OutlineCBData> m_outlineCB;

    IGraphicsDevice* m_gfxPtr = nullptr;
};
