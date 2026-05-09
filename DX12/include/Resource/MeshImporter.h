#pragma once

#include "Resource/IImporter.h"

namespace Resource
{
    // Converts .obj source files to the internal .imsh format.
    //
    // .imsh blob layout:
    //   [AssetHeader (24 B)] [MeshMetadata] [interleaved vertex data] [index data]
    //
    // Vertex layout (32 bytes): float3 position + float3 normal + float2 uv
    // Index type: uint32_t
    // Normals: averaged per shared vertex; flat-generated if OBJ has none.
    class MeshImporter : public IImporter
    {
    public:
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override { return { ".obj" }; }
        const char* GetInternalExtension() const override { return ".imsh"; }
    };
}
