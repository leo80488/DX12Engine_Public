#include "Resource/TextureLoader.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"
#include <DirectXTex.h>

namespace Resource
{
    LoadResult TextureLoader::Load(const std::string& path, const std::vector<uint8_t>& data)
    {
        if (data.empty())
        {
            LOG_ERROR("TextureLoader: empty data for '%s'", path.c_str());
            return {};
        }

        // Validate AssetHeader magic + version + blob size
        if (!ValidateHeader(data.data(), data.size(), MAGIC_TEXTURE))
        {
            LOG_ERROR("TextureLoader: invalid .itex header in '%s'", path.c_str());
            return {};
        }

        const AssetHeader*    hdr     = GetHeader(data.data());
        const uint8_t*        ddsData = GetPayload(data.data());
        const size_t          ddsSize = hdr->dataSize;

        // Parse the DDS payload
        DirectX::TexMetadata  metadata{};
        DirectX::ScratchImage image;
        HRESULT hr = DirectX::LoadFromDDSMemory(ddsData, ddsSize,
                                                 DirectX::DDS_FLAGS_NONE, &metadata, image);
        if (FAILED(hr))
        {
            LOG_ERROR("TextureLoader: LoadFromDDSMemory failed for '%s' (hr=0x%08X)",
                      path.c_str(), static_cast<unsigned>(hr));
            return {};
        }

        auto resource = std::make_unique<TextureResource>(metadata, std::move(image));
        return { std::move(resource), nullptr };
    }
}
