#pragma once

#include "Resource/IResourceLoader.h"
#include "Resource/ShaderResource.h"
#include "Graphics/IGraphicsDevice.h"
#include <memory>

namespace Resource
{
    // Loads the internal .ishdr format produced by ShaderImporter.
    // Does NOT compile HLSL — D3DCompile runs in ShaderImporter.
    //
    // Registration:
    //   resourceManager.RegisterLoader(std::make_shared<ShaderLoader>(&gfx));
    class ShaderLoader : public IResourceLoader
    {
    public:
        explicit ShaderLoader(IGraphicsDevice* gfx);

        LoadResult  Load(const std::string& path, const std::vector<uint8_t>& data) override;
        const char* GetExtension() const override { return ".ishdr"; }

    private:
        IGraphicsDevice* m_gfx = nullptr;
    };
}
