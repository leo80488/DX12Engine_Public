#pragma once

// AnimationImporter — Assimp-based importer for standalone animation clips.
//
// Converts FBX / GLTF / GLB / DAE / BVH source files to the internal .ianim
// binary format.  Mesh and skeleton data in the source file are ignored; only
// aiAnimation channels are extracted.
//
// .ianim payload layout (see AnimationImporter.cpp for full detail):
//   [AssetHeader][AnimationMetadata]
//   For each clip:
//     uint32 channelCount, frameCount; float duration, frameRate; char name[64]
//     For each channel: char boneName[64]; float3[frameCount]; float4[frameCount]; float3[frameCount]
//     uint32 eventCount; { float time; uint32 nameHash }[]
//
// Registered with ResourceManager for: .fbx .gltf .glb .dae .bvh
// Produces: .ianim

#include "Resource/IImporter.h"

namespace Resource
{
    class AnimationImporter : public IImporter
    {
    public:
        // Converts raw source bytes to a .ianim blob.
        // Returns empty vector if the source file has no animations.
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override
        {
            return { ".fbx", ".gltf", ".glb", ".dae", ".bvh" };
        }

        const char* GetInternalExtension() const override { return ".ianim"; }
    };
}
