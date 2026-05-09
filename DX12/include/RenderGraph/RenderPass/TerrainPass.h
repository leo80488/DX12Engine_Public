#pragma once

// TerrainPass — Mesh-shader heightmap-displaced terrain (Phase 1).
//
// Runs AFTER GBufferPass and writes into the same five GBuffer RTVs +
// shared depth so terrain pixels participate in deferred lighting without
// any special case downstream. No clears (GBufferPass already cleared);
// depth test is GREATER_EQUAL (reversed-Z) and depth-write enabled.
//
// Pipeline:
//   - AS (Terrain.as.hlsl) — one thread per sub-tile, frustum-culls against
//     the camera planes published in TerrainCB, emits surviving sub-tile
//     indices to the MS via payload.
//   - MS (Terrain.ms.hlsl) — one group per visible sub-tile, 12×12 vertex
//     grid (144 verts, 242 prims) fits in a single meshlet.
//   - PS (Terrain.ps.hlsl) — full PBR per-layer blend (albedo / normal /
//     ARM) gated by per-layer world-meter height range × slope range.
//   - No quadtree, no distance LOD, no splatmap-required path (splatmap
//     stays optional — when unset the auto-blend gates pick the layer).
//   - Heightmap is sampled at t2 space0 via the per-draw SRV table (root
//     param 10) which already has D3D12_SHADER_VISIBILITY_ALL — reachable
//     from MS+PS without modifying the global root signature.
//
// Renderer wiring:
//   - Renderer iterates entities with TerrainComponent and uploads the
//     active tile's bounds + heightmap-UV remap into m_terrainCB.
//   - Renderer publishes the CB under the name "TerrainParams" via the
//     RenderContext so the pass can read it without holding a Renderer*.
//   - Renderer hands the heightmap SRV gpu handle to the pass via
//     SetActiveTile(); a 0 handle means "skip dispatch this frame".

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

namespace RG { class RenderContext; }

class TerrainPass : public RG::RenderPass
{
public:
    TerrainPass(RG::RGTextureHandle albedo,
                RG::RGTextureHandle normal,
                RG::RGTextureHandle surface,
                RG::RGTextureHandle depth,
                RG::RGTextureHandle velocity,
                RG::RGTextureHandle emissive);

    const char* GetName() const override { return "TerrainPass"; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // Hot-reload — base default impl walks the shader library accessor.
    ShaderLibrary* GetReloadableShaderLibrary() override { return &m_shaderLib; }

    // Per-frame configuration written by Renderer before the graph executes.
    //
    // heightmapSRV : t2 space0 — required (MS samples it for displacement).
    //                0 means "no terrain entity this frame, skip dispatch".
    // splatmapSRV  : t3 space0 — optional. 0 ⇒ PS falls back to slope debug.
    // dispatchAsGroupCount : how many Amplification Shader thread groups
    //                to dispatch. Each AS group covers AS_GROUP_SIZE
    //                sub-tiles (currently 32) and emits 0..32 MS groups
    //                after frustum culling. Renderer computes
    //                ceil(tilesPerSide² / AS_GROUP_SIZE).
    struct TileBindings
    {
        uint64_t heightmapSRV         = 0;
        uint64_t splatmapSRV          = 0;
        uint32_t dispatchAsGroupCount = 0;
    };
    void SetActiveTile(const TileBindings& b) { m_tile = b; }

    // Read-only accessor — ShadowPass uses this to dispatch a depth-only
    // copy of the terrain into each CSM cascade with the same heightmap
    // SRV the colour pass is using this frame. dispatchAsGroupCount==0 or
    // heightmapSRV==0 means "skip terrain shadow".
    const TileBindings& GetTileBindings() const { return m_tile; }

private:
    bool BuildPSO(IGraphicsDevice& gfx);

    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;
    int                 m_clampSamplerIdx = -1;   // s0 — heightmap/splatmap (UV ∈ [0,1])
    int                 m_wrapSamplerIdx  = -1;   // s1 — layer albedos sampled at world XZ × tiling

    RG::RGTextureHandle m_albedo;
    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_velocity;
    RG::RGTextureHandle m_emissive;

    // Per-frame state pushed by Renderer.
    TileBindings m_tile{};

    // Cached: device feature query result. Init records false when the
    // GPU/runtime does not support D3D12_MESH_SHADER_TIER_1; Execute then
    // becomes a silent no-op so the rest of the engine still runs.
    bool m_meshShaderAvailable = false;
};
