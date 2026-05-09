#pragma once

// AnimationLoader — loads .ianim blobs into AnimationResource objects.
// Registered with ResourceManager for the ".ianim" extension.
// No GPU upload needed (ClipAsset data is CPU-only).

#include "Resource/IResourceLoader.h"

namespace Resource
{
    class AnimationLoader : public IResourceLoader
    {
    public:
        LoadResult Load(const std::string& path,
                        const std::vector<uint8_t>& data) override;

        const char* GetExtension() const override { return ".ianim"; }
    };
}
