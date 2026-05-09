#pragma once

#include "Resource/IResourceLoader.h"
#include "Resource/TextureResource.h"
#include <memory>

namespace Resource
{
    // Loads the internal .itex format produced by TextureImporter.
    // Does NOT handle external formats (PNG, JPG, TGA) — those go through TextureImporter first.
    //
    // Registration:
    //   resourceManager.RegisterLoader(std::make_shared<TextureLoader>());
    class TextureLoader : public IResourceLoader
    {
    public:
        TextureLoader() = default;

        LoadResult  Load(const std::string& path, const std::vector<uint8_t>& data) override;
        const char* GetExtension() const override { return ".itex"; }
    };
}
