#pragma once

#include "Resource/IResourceLoader.h"
#include "Resource/TextureResource.h"
#include <memory>

namespace Resource
{
    /**
     * DDS Texture loader：直接從 .dds 檔案讀入，使用 DirectXTex::LoadFromDDSMemory，
     * 產生 CPU 端的 TextureResource（TexMetadata + ScratchImage）。
     *
     * 建議在 ResourceManager 初始化時註冊：
     *   resourceManager.RegisterLoader(std::make_shared<DDSTextureLoader>());
     */
    class DDSTextureLoader : public IResourceLoader
    {
    public:
        DDSTextureLoader() = default;

        LoadResult Load(const std::string& path,
                        const std::vector<uint8_t>& data) override;

        const char* GetExtension() const override { return ".dds"; }
    };
}

