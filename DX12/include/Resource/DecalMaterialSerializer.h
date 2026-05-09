#pragma once

// DecalMaterialSerializer — JSON persistence for DecalMaterialAsset.
//
// Format: plain .idecalmat JSON file. Chose JSON over the binary+text hybrid
// used for MaterialComponent (.imat) because:
//   - Decal materials have ~10 fields — no size pressure.
//   - Hand-diffable / git-friendly.
//   - Easier for tools (content pipelines, hot-reload watchers).
//
// Schema (v1):
// {
//   "version": 1,
//   "name":    "blood_splat_a",
//   "textures": {
//     "albedo": "asset/Decals/blood_albedo.itex",
//     "normal": "asset/Decals/blood_normal.itex",
//     "orm":    "asset/Decals/blood_orm.itex"
//   },
//   "tint":            [1.0, 0.25, 0.15, 1.0],
//   "opacity":         1.0,
//   "flags":           15,            // DECAL_WRITE_* bitmask
//   "normalBlendMode": "RNM",         // or "Lerp"
//   "angleFadeStart":  0.3,
//   "sortLayer":       0
// }

#include <string>

namespace Resource
{
    class DecalMaterialAsset;

    // Write @p asset to @p path as JSON. Returns false on I/O error.
    bool SaveDecalMaterial(const DecalMaterialAsset& asset, const std::string& path);

    // Populate @p asset from the JSON file at @p path. Only the editor-owned
    // fields are overwritten — runtime caches (handles, bindless indices) are
    // left for the next Resolve() / TryPromote() cycle to fill. Returns false
    // on parse/IO error; the asset is left unmodified in that case.
    bool LoadDecalMaterial(DecalMaterialAsset& asset, const std::string& path);
}
