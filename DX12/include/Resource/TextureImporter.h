#pragma once

#include "Resource/IImporter.h"

namespace Resource
{
    // Converts external image formats (PNG, JPG, TGA, BMP, HDR) to the internal
    // .itex format.
    //
    // .itex blob layout:
    //   [AssetHeader (20 B)] [TextureMetadata] [DDS bytes]
    //
    // The DDS payload is generated via DirectX::SaveToDDSMemory, making the
    // internal format GPU-friendly from the start.  TextureLoader reads .itex
    // by calling DirectX::LoadFromDDSMemory on the payload.
    class TextureImporter : public IImporter
    {
    public:
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override
        {
            return { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" };
        }

        const char* GetInternalExtension() const override { return ".itex"; }
    };
}
