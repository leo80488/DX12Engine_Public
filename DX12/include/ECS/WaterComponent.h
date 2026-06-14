#pragma once

// WaterComponent — one flat water tile rendered by WaterPass (forward,
// after SkyboxPass): Fresnel blend between an absorption-tinted body colour
// and the sky cubemap + a Blinn sun glint. Surface detail comes from TWO
// tangent-space normal flow maps counter-scrolled along flowDir and
// whiteout-blended (map B at a finer tiling for detail). Paths are editor
// drag-droppable (ITEX_PATH); an empty/unloaded path falls back to the
// pass-owned flat normal (calm water).
//
// Water depth (absorption + shoreline fade) is computed analytically from
// the scene's (first) TerrainComponent heightmap; without terrain the water
// renders at full depth everywhere.
//
// Renderer/BuildScene_SyncWater (Renderer_GrassWater.cpp) syncs this each
// frame (texture acquire/release on path change included).
// Singleton-by-convention: only the first live WaterComponent renders.

#include "Resource/SystemHandles.h"   // TextureHandle / kInvalidTextureHandle

#include <DirectXMath.h>
#include <cstdint>
#include <string>

struct WaterComponent
{
    bool enabled = true;

    // ---- Placement: worldCenter.y IS the water level -------------------------
    DirectX::XMFLOAT3 worldCenter { 0.0f, 0.0f, 0.0f };
    float             worldSize   = 1024.0f;

    // ---- Body colour / depth response ------------------------------------------
    DirectX::XMFLOAT3 deepColor    { 0.012f, 0.07f, 0.14f };  // linear
    DirectX::XMFLOAT3 shallowColor { 0.10f,  0.28f, 0.26f };
    float absorbDist = 6.0f;   // metres of water to reach the deep colour
    float shoreFade  = 1.2f;   // metres of alpha fade at the shoreline

    // ---- Flow normals --------------------------------------------------------------
    // Two tangent-space normal maps; A is the broad swell layer, B scrolls
    // against the flow at a finer tiling for surface detail.
    std::string normalMapAPath = "asset/Default_Texture/water/Water 1_normal.itex";
    std::string normalMapBPath = "asset/Default_Texture/water/Water 2_normal.itex";

    DirectX::XMFLOAT2 flowDir { 1.0f, 0.35f };  // normalized at sync
    float flowSpeed      = 0.55f;   // m/s scroll
    float normalTiling   = 0.35f;   // map A repeats per metre (B tiles 2.3×)
    float normalStrength = 0.45f;

    // Renderer-internal — populated by SyncWater each frame.
    mutable Resource::TextureHandle normalMapAHandle = Resource::kInvalidTextureHandle;
    mutable Resource::TextureHandle normalMapBHandle = Resource::kInvalidTextureHandle;
    mutable uint64_t                normalMapASRV    = 0;
    mutable uint64_t                normalMapBSRV    = 0;

    // ---- Shading ----------------------------------------------------------------------
    float fresnelF0    = 0.02f;   // water IOR 1.33
    float reflStrength = 1.0f;    // sky reflection multiplier
    float specPower    = 260.0f;  // sun glint tightness

    // ---- Mesh ------------------------------------------------------------------------------
    uint32_t gridQuads = 128;     // grid resolution per side
};
