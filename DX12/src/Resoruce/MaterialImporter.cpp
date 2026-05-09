#include "Resource/MaterialImporter.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"

#include <cstring>

namespace Resource
{
    std::vector<uint8_t> MaterialImporter::Import(const std::string& sourcePath,
                                                    const std::vector<uint8_t>& sourceData)
    {
        if (sourceData.empty())
        {
            LOG_ERROR("MaterialImporter: empty source data for '%s'", sourcePath.c_str());
            return {};
        }

        // Payload: raw text bytes + null terminator
        const uint32_t textLength  = static_cast<uint32_t>(sourceData.size());
        const uint32_t payloadSize = textLength + 1;  // +1 for null terminator

        MaterialMetadata matMeta{};
        matMeta.textLength   = textLength;
        matMeta.reserved[0]  = 0;
        matMeta.reserved[1]  = 0;
        matMeta.reserved[2]  = 0;

        AssetHeader header{};
        header.magic        = MAGIC_MATERIAL;
        header.version      = ASSET_VERSION;
        header.resourceType = static_cast<uint16_t>(ResourceType::Material);
        header.metadataSize = sizeof(MaterialMetadata);
        header.dataSize     = payloadSize;
        header.flags        = 0;
        header.reserved     = 0;

        const size_t totalSize = sizeof(AssetHeader) + sizeof(MaterialMetadata) + payloadSize;
        std::vector<uint8_t> blob(totalSize, 0);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header,  sizeof(AssetHeader));     dst += sizeof(AssetHeader);
        std::memcpy(dst, &matMeta, sizeof(MaterialMetadata)); dst += sizeof(MaterialMetadata);
        std::memcpy(dst, sourceData.data(), textLength);
        // last byte stays 0 (null terminator, blob was zero-initialised)

        LOG_INFO("MaterialImporter: imported '%s' → .imat (%u bytes text, %zu bytes total)",
                 sourcePath.c_str(), textLength, totalSize);

        return blob;
    }
}
