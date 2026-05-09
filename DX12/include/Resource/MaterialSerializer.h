#pragma once

// MaterialSerializer — save/load MaterialComponent to/from the .imat binary format.
//
// .imat blob layout (identical to MaterialImporter output):
//   [AssetHeader (24 B)] [MaterialMetadata (16 B)] [key=value text payload (null-terminated)]
//
// Key=value text keys:
//   shaderType, blendMode, castShadow, receiveShadow, doubleSided, outline
//   roughnessMax, roughnessMin, metalnessMax, metalnessMin, reflectance,
//   normalMapStrength, saturation, alphaRef
//   baseColor, specularColor, emissiveColor  (format: "r g b a")
//   texTiling (format: "u v"), texOffset (format: "u v")
//   tex_BASECOLORMAP, tex_NORMALMAP, tex_SURFACEMAP, tex_METALLICMAP,
//   tex_ROUGHNESSMAP, tex_OCCLUSIONMAP, tex_EMISSIVEMAP

#include <string>
#include <vector>
#include <cstdint>

struct MaterialComponent;

namespace Resource
{
    // Write mat to an .imat file at path. Returns true on success.
    bool SaveMaterial(const MaterialComponent& mat, const std::string& path);

    // Read an .imat file at path and apply its contents to mat.
    // Marks mat dirty on success. Returns true on success.
    bool LoadMaterial(const std::string& path, MaterialComponent& mat);

    // Build an .imat binary blob in memory (for BuildBlobs pipelines).
    // Returns the blob bytes. Caller appends to their file list.
    std::vector<uint8_t> BuildMaterialBlob(const MaterialComponent& mat);
}
