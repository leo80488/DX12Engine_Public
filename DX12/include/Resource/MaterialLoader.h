#pragma once

#include "Resource/IResourceLoader.h"

namespace Resource
{
    // Loads internal .imat blobs produced by MaterialImporter.
    // Parses the embedded key=value text payload and returns a MaterialResource.
    //
    // Registration:
    //   resourceManager.RegisterLoader(std::make_shared<MaterialLoader>());
    class MaterialLoader : public IResourceLoader
    {
    public:
        MaterialLoader() = default;

        LoadResult  Load(const std::string& path, const std::vector<uint8_t>& data) override;
        const char* GetExtension() const override { return ".imat"; }
    };
}
