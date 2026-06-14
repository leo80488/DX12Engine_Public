#pragma once

// TerrainComponent — ECS marker for a heightmap-displaced mesh-shader terrain.
//
// Phase 1 (single-tile, no LOD, no quadtree):
//   - One TerrainComponent per scene entity = one square tile.
//   - Heightmap sampled per-vertex inside the MS; flat fallback when unset.
//
// Phase 2 (this file): splatmap + 4-layer PBR blend.
//   - Splatmap RGBA channels weight 4 layer textures.
//   - Each layer is a Texture2D (bindless) sampled by world-XZ × tilingScale.
//   - Layer normal/MRA arrays land in Phase 3 alongside reflection-driven
//     param exposure through MaterialReflectionSync.
//
// Renderer/BuildScene_SyncTerrain syncs every texture path through
// TextureSystem (acquire/release on path change) and uploads the per-frame
// TerrainCB before the graph executes.

#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"   // TextureHandle / kInvalidTextureHandle
#include "Resource/HeightField.h"     // CPU-side height grid for collision

#include <array>
#include <memory>
#include <string>
#include <vector>
#include <DirectXMath.h>

// One PBR layer in a terrain splat blend. Phase 3 ships full PBR:
//   albedoPath  — diffuse / albedo (RGB)
//   normalPath  — tangent-space normal (DX convention: Y-up)
//   armPath     — packed AO / Roughness / Metalness (R / G / B)
//   dispPath    — single-channel height; biases the splat blend so layer
//                 transitions follow the micro-relief (height-correlated
//                 blend). Only consulted when TerrainComponent.heightBlend
//                 is enabled; missing → that layer contributes 0 bias.
// When a splatmap is set, the first 4 layers map to its RGBA channels
// (layer0 = R, 1 = G, 2 = B, 3 = A); the auto-blend path supports any count.
struct TerrainLayer
{
    std::string albedoPath;
    std::string normalPath;
    std::string armPath;
    std::string dispPath;

    // World-space tiling — UV repeats per world meter. 0.05 means one
    // texture repeat every 20 m, a sensible default for ground textures.
    float       tilingScale = 0.05f;

    // ---------------------------------------------------------------------
    // Auto-blend (no splatmap path): each layer is gated by a WORLD-SPACE
    // height range AND a slope range — both in directly authorable units.
    // The two gates multiply, so a layer only shows where BOTH conditions
    // are met. This replaces the old normalised "preferredAltitude" +
    // global "bandWidth" — you no longer need to think in [0,1] heightmap
    // space, you just say "grass between 0 m and 200 m, on slopes 0–35°".
    //
    // Height gate:
    //   weight = 1                       inside [minHeight, maxHeight]
    //   weight = 0                       outside [minHeight - fadeHeight,
    //                                              maxHeight + fadeHeight]
    //   smoothstep falloff in the fade band (cubic, no hard edges).
    //
    // Slope gate:
    //   weight = 1                       inside [minSlope, maxSlope] (deg)
    //   weight = 0                       outside [minSlope - fadeSlope,
    //                                              maxSlope + fadeSlope]
    // Slope is the angle between the surface normal and world up:
    //   0°  = perfectly horizontal (flat ground)
    //   90° = vertical cliff
    //
    // Defaults map cleanly to a typical low→high stack on a 200 m tile:
    //   layer 0 = grass    (low-mid altitude, flat ground)
    //   layer 1 = rocks    (mid altitude, steep slopes)
    //   layer 2 = pebbles  (low altitude / riverbed)
    //   layer 3 = snow     (peaks)
    // Inspector edits these live; ignored when a splatmap is set.
    // ---------------------------------------------------------------------
    // Defaults are deliberately WIDE OPEN — a fresh layer is visible
    // everywhere. Narrow `min/maxHeight` and `min/maxSlopeDeg` in the
    // inspector to gate it to a band; that's the intuitive direction
    // (start permissive, restrict by hand). Layers with disjoint gates
    // produce clean per-zone looks; overlapping gates cross-fade.
    float       minHeight    = -1e6f;    // world m — layer fully visible above this height
    float       maxHeight    =  1e6f;    // world m — layer fully visible below this height
    float       fadeHeight   =  10.0f;   // world m — soft falloff width on both ends

    float       minSlopeDeg  =   0.0f;   // 0° = flat horizontal
    float       maxSlopeDeg  =  90.0f;   // 90° = vertical cliff
    float       fadeSlopeDeg =   5.0f;   // soft falloff width on both ends

    // Renderer-internal — populated by SyncTerrain each frame.
    // bindlessIdx == -1 indicates the texture is not yet GPU-resident;
    // PS skips that map (per-channel fallback) until promotion.
    mutable Resource::TextureHandle albedoHandle      = Resource::kInvalidTextureHandle;
    mutable Resource::TextureHandle normalHandle      = Resource::kInvalidTextureHandle;
    mutable Resource::TextureHandle armHandle         = Resource::kInvalidTextureHandle;
    mutable Resource::TextureHandle dispHandle        = Resource::kInvalidTextureHandle;
    mutable int32_t                 albedoBindlessIdx = -1;
    mutable int32_t                 normalBindlessIdx = -1;
    mutable int32_t                 armBindlessIdx    = -1;
    mutable int32_t                 dispBindlessIdx   = -1;
};

struct TerrainComponent
{
    // Path to the heightmap asset (.itex, .dds, .png — anything TextureSystem
    // can resolve). When empty the tile renders flat.
    std::string heightmapPath;

    // World-space tile placement.
    //   X ∈ [worldCenter.x - worldSize*0.5, worldCenter.x + worldSize*0.5]
    //   Z ∈ [worldCenter.z - worldSize*0.5, worldCenter.z + worldSize*0.5]
    // worldCenter.y is the BASE / floor of the terrain and heightScale is
    // the TOTAL RELIEF (valley floor → highest peak):
    //   Y ∈ [worldCenter.y, worldCenter.y + heightScale]
    // The Renderer re-anchors the heightmap's ACTUAL data range onto that
    // span (see Renderer_Terrain.cpp "height-range re-anchoring"): the
    // lowest sampled value maps to worldCenter.y, the highest to
    // worldCenter.y + heightScale. So bumping heightScale only grows the
    // peaks upward — the valley floor stays pinned at worldCenter.y even
    // when the heightmap doesn't use the full [0,1] encodable range (most
    // don't; raw mapping would translate the whole tile by
    // dataMin × heightScale on every scale edit). Until the CPU HeightField
    // is decoded (R16_UNORM only) the raw mapping is used as a fallback.
    // Equivalently, bumping worldSize keeps the tile centred on
    // (worldCenter.x, worldCenter.z) — the pivot is stable while you tune
    // the macro shape.
    DirectX::XMFLOAT3 worldCenter   { 0.0f, 0.0f, 0.0f };
    float             worldSize     = 1024.0f;
    float             heightScale   = 100.0f;

    // Sub-tile grid count per side. The macro tile is divided into a
    // tilesPerSide × tilesPerSide grid; one mesh-shader thread group emits
    // each sub-tile as a 12×12 vertex patch (≈11×11 quads), the maximum
    // a single MS group can produce within DX12's 256-vert / 256-prim
    // budget. Going finer means dispatching more groups, not bigger ones.
    //
    // Effective per-quad world size = worldSize / (tilesPerSide * 11):
    //   tilesPerSide=32  → ~11.6 m per quad at worldSize=4096
    //   tilesPerSide=64  →  ~5.8 m
    //   tilesPerSide=128 →  ~2.9 m   (≈1M triangles for the whole tile)
    //   tilesPerSide=192 →  ~1.9 m
    //   tilesPerSide=256 →  ~1.5 m   (≈4M triangles)
    //
    // The PS computes the geometric normal per-pixel from heightmap
    // derivatives, so shading stays smooth regardless of mesh density —
    // this only controls GEOMETRIC silhouette fidelity (skylines, ridges).
    // Cost grows as N²; values up to ~256 are still cheap on modern GPUs.
    uint32_t tilesPerSide = 128;

    // Heightmap UV remap. Defaults to identity (the whole texture covers the
    // whole tile). Use these when one heightmap drives many tiles.
    DirectX::XMFLOAT2 heightmapUVOffset { 0.0f, 0.0f };
    DirectX::XMFLOAT2 heightmapUVScale  { 1.0f, 1.0f };

    // Splatmap (Texture2D, RGBA). Each channel is the weight of one layer.
    // When unset the PS falls back to the Phase-1 slope-based debug colour
    // so terrain stays visible even before splatmap authoring is done.
    std::string splatmapPath;

    // PBR layers blended by splatmap RGBA (first 4 channels) or by the
    // per-layer height/slope auto-blend (any count). Count is data-driven —
    // the GPU loops over layers.size() via a StructuredBuffer, so adding /
    // removing a layer is a pure data edit (no shader/CB/struct changes).
    // Defaults to 4 entries to match the historical fixed-4 authoring.
    std::vector<TerrainLayer> layers = std::vector<TerrainLayer>(4);

    // ---- Height-correlated blend (per-layer displacement) -------------------
    // When enabled, each layer's blend weight is biased by its disp map so
    // transitions follow the underlying micro-relief instead of a flat lerp
    // ("grass pokes through where the gravel dips"). Off by default → identical
    // to the plain linear splat/auto blend, so existing scenes are unchanged.
    //   strength — how strongly disp biases the base weight. Small (0.1–0.2)
    //              keeps splat/gate dominant; large lets disp override.
    //   range    — soft cutoff width: a layer keeps weight while its biased
    //              value is within `range` of the local maximum. Wider =
    //              smoother multi-layer blends; narrower = sharper interlock.
    bool  heightBlendEnabled  = false;
    float heightBlendStrength = 0.15f;
    float heightBlendRange    = 0.10f;

    // Renderer-internal — populated by SyncTerrain each frame.
    mutable Resource::TextureHandle heightmapHandle = Resource::kInvalidTextureHandle;
    mutable uint64_t                heightmapSRV    = 0;
    mutable Resource::TextureHandle splatmapHandle  = Resource::kInvalidTextureHandle;
    mutable uint64_t                splatmapSRV     = 0;

    // Renderer-internal — the RE-ANCHORED world-Y mapping actually fed to
    // the GPU (TerrainParamsCB) and CPU (HeightField) this frame:
    //   worldY = effBaseY + rawSample01 * effHeightScale
    // Equal to (worldCenter.y, heightScale) until the heightmap's data range
    // is known, then adjusted so the data minimum lands on worldCenter.y.
    // Grass/Water sync read these so every height consumer agrees.
    mutable float effBaseY        = 0.0f;
    mutable float effHeightScale  = 0.0f;

    // CPU-side height grid for collision / queries. Renderer decodes this
    // out of the loaded R16_UNORM heightmap the first frame the texture
    // becomes ready, and refreshes it whenever the heightmap path or the
    // worldCenter.y / heightScale change. Shared via shared_ptr so future
    // multi-tile setups can re-use one heightmap across many tiles
    // without each entity holding its own copy. Null until ready.
    mutable std::shared_ptr<Resource::HeightField> heightField;
};

// ---------------------------------------------------------------------------
// SampleTerrainHeightAt — collision-side height query.
//
// Given a world-XZ point (and a TerrainComponent that owns a HeightField),
// returns the world Y of the terrain surface at that point. Reproduces the
// SAME math the GPU uses inside Terrain.ms.hlsl / Terrain.ps.hlsl so a
// physics entity standing on the rendered surface ends up at exactly the
// rendered Y — no float drift between visible mesh and collision.
//
//   1. world XZ → normalised tile-local position [0,1]² using the
//      worldOrigin = worldCenter - worldSize/2 convention.
//   2. apply heightmapUVOffset / heightmapUVScale (matches the GPU UV
//      remap so multi-tile setups sharing one heightmap stay consistent).
//   3. bilinear-sample the HeightField → world Y in [baseY, baseY+heightScale].
//
// Returns `outOfBounds` (defaulted to baseY) when:
//   - heightField is null / not yet decoded;
//   - the queried XZ falls outside the tile.
// Caller should test whether (worldX, worldZ) is inside the tile bounds
// before relying on the result for collision response.
inline float SampleTerrainHeightAt(const TerrainComponent& tc,
                                   float worldX, float worldZ,
                                   bool* outInBounds = nullptr)
{
    const auto& hf = tc.heightField;
    if (!hf || !hf->IsValid())
    {
        if (outInBounds) *outInBounds = false;
        return tc.worldCenter.y;
    }

    const float halfSize = tc.worldSize * 0.5f;
    const float originX  = tc.worldCenter.x - halfSize;
    const float originZ  = tc.worldCenter.z - halfSize;

    const float u = (worldX - originX) / tc.worldSize;
    const float v = (worldZ - originZ) / tc.worldSize;

    const bool inBounds = (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
    if (outInBounds) *outInBounds = inBounds;

    // Apply the same UV remap the GPU uses, so a heightmap tiled over
    // many entities samples consistently.
    const float hu = tc.heightmapUVOffset.x + u * tc.heightmapUVScale.x;
    const float hv = tc.heightmapUVOffset.y + v * tc.heightmapUVScale.y;

    return hf->SampleHeightWorld(hu, hv);
}
