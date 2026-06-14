#pragma once

// GrassPass — GoT-style procedural grass blades (mesh-shader pipeline).
//
// Runs right after TerrainPass and writes into the same five GBuffer RTVs +
// HDR emissive + shared depth, stencil ref 1 (PBR), so grass participates in
// deferred lighting / CSM shadows / DDGI / SSAO with no special cases
// downstream.
//
// Pipeline (see Grass.hlsli for the full design notes):
//   - AS (Grass.as.hlsl)  — one thread per grass patch: frustum cull
//     (conservative AABB, same p-vertex test as Terrain.as), distance cull
//     with dissolve fade, LOD pick (blade segments / blades-per-group /
//     density), group-scope prefix scan assigns MS group ranges (per-lane
//     payload, wave-width agnostic).
//   - MS (Grass.ms.hlsl)  — N blades per group, fully procedural quadratic
//     Bezier ribbons: hash placement (bit-reversed stratified slots so
//     positions are stable across density changes), terrain-heightmap root
//     anchoring (bicubic — identical math to the terrain MS), Voronoi-lite
//     clump identity, scrolling wind noise re-evaluated at prevTime for TAA
//     velocity, per-blade world-Y / slope gates.
//   - PS (Grass.ps.hlsl)  — GBuffer write (albedo gradient, softened normal,
//     roughness/AO, velocity, zero emissive).
//
// No vertex/instance/indirect buffers exist — blade data never touches
// memory. Grass does not cast shadows (deliberate; it receives CSM via the
// deferred resolve).
//
// Renderer wiring (Renderer_GrassWater.cpp / BuildScene_SyncGrass):
//   - fills GrassCBData from the first GrassComponent + first
//     TerrainComponent's heightmap mapping, publishes the CB as
//     "GrassParams", and arms the pass via SetActiveField().
//   - heightmapSRV == 0 → flat field at baseY using the pass-owned 1×1
//     fallback heightmap (the shader skips sampling via g_hasHeightmap).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

namespace RG { class RenderContext; }

// HLSL-layout mirror of Grass.hlsli's GrassCB (b2 space0). 20 rows × 16 B.
struct alignas(16) GrassCBData
{
    // row 0
    float    grassOriginX, grassOriginZ;
    float    grassSize;
    float    baseY;
    // row 1
    float    heightScale;
    float    hmTexel;
    uint32_t hasHeightmap;
    uint32_t patchesPerSide;
    // row 2
    float    hmUVOffsetX, hmUVOffsetY;
    float    hmUVScaleX,  hmUVScaleY;
    // row 3
    float    terrainOriginX, terrainOriginZ;
    float    terrainSize;
    float    _padT;
    // row 4
    float    cameraPos[3];
    float    time;
    // row 5
    float    prevTime;
    float    lod0Dist;
    float    lod1Dist;
    float    cullDist;
    // row 6
    float    density;
    float    bladeHeight;
    float    bladeHeightVar;
    float    bladeWidth;
    // row 7
    float    tiltMax;           // radians
    float    bendAmount;
    float    windStrength;
    float    windSpeed;
    // row 8
    float    windScale;
    float    windDirX, windDirZ;
    float    clumpCellSize;
    // row 9
    float    clumpBlend;
    float    minWorldY;
    float    maxWorldY;
    float    maxSlopeCos;
    // rows 10/11
    float    baseColor[4];
    float    tipColor[4];
    // row 12
    float    colorNoiseScale;
    float    colorNoiseAmount;
    float    rootAO;
    float    normalBlend;
    // row 13
    float    viewThicken;
    float    farWidthMul;
    float    roughness;
    uint32_t seed;
    // rows 14..19
    float    frustumPlanes[6][4];
};
static_assert(sizeof(GrassCBData) == 320,
    "GrassCBData layout drift — sync Grass.hlsli cbuffer GrassCB");

class GrassPass : public RG::RenderPass
{
public:
    GrassPass(RG::RGTextureHandle albedo,
              RG::RGTextureHandle normal,
              RG::RGTextureHandle surface,
              RG::RGTextureHandle depth,
              RG::RGTextureHandle velocity,
              RG::RGTextureHandle emissive);

    const char* GetName() const override { return "GrassPass"; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    ShaderLibrary* GetReloadableShaderLibrary() override { return &m_shaderLib; }

    // Per-frame state pushed by Renderer (BuildScene_SyncGrass) before the
    // graph executes. dispatchAsGroupCount == 0 → skip (no grass entity).
    struct FieldBindings
    {
        uint64_t heightmapSRV         = 0;   // 0 → pass-owned 1×1 fallback
        uint32_t dispatchAsGroupCount = 0;
    };
    void SetActiveField(const FieldBindings& b, const GrassCBData& cb)
    {
        m_field     = b;
        m_pendingCB = cb;
    }

    // Global view-mode (Renderer::SetViewMode) — wireframe PSO swap.
    void SetWireframe(bool w) { m_wireframe = w; }

    // Current-frame slot of the triple-buffered GrassCB; Renderer registers
    // it with the RenderGraph each frame under the name "GrassParams".
    const RHI::GPUBuffer& GetGrassCB(IGraphicsDevice& gfx) const
    {
        return m_grassCB.CurrentBuffer(gfx);
    }

private:
    bool BuildPSO(IGraphicsDevice& gfx, bool wireframe, RHI::PipelineState& outPso);

    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;
    RHI::PipelineState  m_psoWire;
    bool                m_wireframe = false;
    int                 m_clampSamplerIdx = -1;   // s0 — heightmap

    FrameCB<GrassCBData> m_grassCB;
    GrassCBData          m_pendingCB{};

    // 1×1 R16_UNORM fallback so root slot 10 always holds a valid Texture2D
    // descriptor even when no terrain heightmap exists/is loaded.
    RHI::Texture        m_fallbackHeightmap;
    uint64_t            m_fallbackHeightmapSRV = 0;

    RG::RGTextureHandle m_albedo;
    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_velocity;
    RG::RGTextureHandle m_emissive;

    FieldBindings m_field{};

    bool m_meshShaderAvailable = false;
};
