#include "Resource/DDSTextureLoader.h"
#include "System/Log.h"
#include <DirectXTex.h>

namespace Resource
{
    LoadResult DDSTextureLoader::Load(const std::string& path,
                                      const std::vector<uint8_t>& data)
    {
        if (data.empty())
        {
            LOG_ERROR("DDSTextureLoader: empty data for %s", path.c_str());
            return { nullptr, nullptr };
        }

        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage image;

        HRESULT hr = DirectX::LoadFromDDSMemory(
            data.data(),
            data.size(),
            DirectX::DDS_FLAGS_NONE,
            &metadata,
            image);

        if (FAILED(hr))
        {
            LOG_ERROR("DDSTextureLoader: failed to parse %s (hr=0x%08X)", path.c_str(), static_cast<unsigned>(hr));
            return { nullptr, nullptr };
        }

        auto resource = std::make_unique<TextureResource>(metadata, std::move(image));
        return { std::move(resource), nullptr };
    }
}

