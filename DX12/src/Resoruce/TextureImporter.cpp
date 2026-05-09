#include "Resource/TextureImporter.h"
#include "Resource/AssetHeader.h"
#include "Resource/BCCompressor.h"
#include "System/Log.h"
#include <DirectXTex.h>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace Resource
{
    // -------------------------------------------------------------------------
    // Texture role — drives format / compression selection.
    //
    //   Color          → BC7_UNORM_SRGB
    //   Normal         → BC5_UNORM (RG, B reconstructed in shader)
    //   SingleChannel  → BC4_UNORM (lossy 8-bit per 4×4 block)
    //   HDR            → BC6H_UF16
    //   Other          → BC7_UNORM
    //
    //   --- Uncompressed roles, all NO-BC ---
    //   RawHeight      → R16_UNORM, 1-channel. For heightmaps / displacement
    //                    masks. Linear 16-bit gives the full 65 536 distinct
    //                    levels across [0,1], which is what we want for
    //                    terrain collision (lossless round-trip from the
    //                    source PNG/EXR). R16_FLOAT was used previously but
    //                    only carries 11 bits of mantissa — quantisation
    //                    bunches up at the top of [0,1] and shows up as
    //                    visible normal-stencil banding on bright heights.
    //                    Keywords: "heightmap", "_h16", "_raw_height".
    //   RawLUT2        → R16G16_FLOAT, 2-channel. For BRDF LUTs, IBL split-
    //                    sum tables, etc. — anything where each channel
    //                    needs ≥10-bit linear precision. Keywords: "brdf",
    //                    "_brdf".
    //   RawColor       → R8G8B8A8_UNORM, 4-channel linear. For NPR ramps,
    //                    palette LUTs, and generic "preserve every pixel"
    //                    cases. Keywords: "ramp", "_ramp", "lut", "_lut",
    //                    "_raw", "_uncompressed", "_lossless".
    // -------------------------------------------------------------------------
    enum class TexRole { Color, Normal, SingleChannel, HDR, RawHeight, RawLUT2, RawColor, Other };

    static TexRole DetectRole(const std::string& path, const std::string& ext)
    {
        if (ext == ".hdr") return TexRole::HDR;

        // Lowercase the full path for keyword matching.
        std::string lower = path;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // ---- Uncompressed paths (most specific first) ----------------------
        // Each goes to its own role with the right channel count + format.

        // BRDF LUT / split-sum tables — RG16_FLOAT.
        static const std::vector<std::string> brdfKeywords = {
            "brdf", "_brdf"
        };
        for (const auto& keyword : brdfKeywords)
        {
            if (lower.find(keyword) != std::string::npos)
                return TexRole::RawLUT2;
        }

        // Heightmap / displacement (single channel) — R16_UNORM.
        // Checked BEFORE the SingleChannel ("_height") branch so a heightmap
        // doesn't get pulled into BC4.
        static const std::vector<std::string> heightmapKeywords = {
            "heightmap", "_h16", "_raw_height","_disp"
        };
        for (const auto& keyword : heightmapKeywords)
        {
            if (lower.find(keyword) != std::string::npos)
                return TexRole::RawHeight;
        }

        // Color ramps / generic uncompressed — R8G8B8A8_UNORM (4-channel).
        // For NPR ramps and palette LUTs the per-pixel colour values must be
        // preserved bit-exact, so block compression is unsuitable. Linear
        // (no sRGB tag) so the GPU samples the values you authored.
        static const std::vector<std::string> rawColorKeywords = {
            "ramp", "_ramp", "lut", "_lut",
            "_raw", "_uncompressed", "_lossless"
        };
        for (const auto& keyword : rawColorKeywords)
        {
            if (lower.find(keyword) != std::string::npos)
                return TexRole::RawColor;
        }

        // Normal map keywords (_normal, _nrm, _norm, _n., nmap, normalmap).
        static const std::vector<std::string> normalKeywords = {
            "_normal", "_nrm", "_norm", "_n.", "nmap", "normalmap"
		};
        for(const auto& keyword : normalKeywords)
        {
            if (lower.find(keyword) != std::string::npos)
                return TexRole::Normal;
		}

        static const std::vector<std::string> singleChannelKeywords = {
            "roughness", "_roughness", "metalness", "_metallic", "_ao",
            "ambient", "_height",  "_mask", "_bump",
            "_gloss", "_opacity", "_specular", "_cavity"
        };
        // Single-channel greyscale maps (roughness, metalness, AO, height …).
        for (const auto& keyword : singleChannelKeywords) {
            if (lower.find(keyword) != std::string::npos) {
                return TexRole::SingleChannel;
            }
        }

        // Colour / albedo / base-color / specular highlight (sRGB BC7).
        static const std::vector<std::string> colorKeywords = {
            "albedo", "basecolor", "diffuse", "_color", "_col", "_d.", "_da.", "_spc"
		};
        for (const auto& keyword : colorKeywords)
        {
            if (lower.find(keyword) != std::string::npos)
                return TexRole::Color;
        }

        return TexRole::Other; // BC7_UNORM
    }

    // -------------------------------------------------------------------------
    std::vector<uint8_t> TextureImporter::Import(const std::string& sourcePath,
                                                   const std::vector<uint8_t>& sourceData)
    {
        if (sourceData.empty())
        {
            LOG_ERROR("TextureImporter: empty source data for '%s'", sourcePath.c_str());
            return {};
        }

        // --- Determine source extension ---
        std::string ext;
        {
            size_t pos = sourcePath.find_last_of('.');
            if (pos != std::string::npos)
            {
                ext = sourcePath.substr(pos);
                for (char& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }

        // --- Decode source image ---
        DirectX::TexMetadata  meta{};
        DirectX::ScratchImage image;
        HRESULT hr = E_FAIL;

        if (ext == ".dds")
        {
            hr = DirectX::LoadFromDDSMemory(sourceData.data(), sourceData.size(),
                                             DirectX::DDS_FLAGS_NONE, &meta, image);
        }
        else if (ext == ".tga")
        {
            hr = DirectX::LoadFromTGAMemory(sourceData.data(), sourceData.size(),
                                             DirectX::TGA_FLAGS_NONE, &meta, image);
        }
        else if (ext == ".hdr")
        {
            hr = DirectX::LoadFromHDRMemory(sourceData.data(), sourceData.size(),
                                             &meta, image);
        }
        else
        {
            // PNG, JPG, JPEG, BMP → WIC
            hr = DirectX::LoadFromWICMemory(sourceData.data(), sourceData.size(),
                                             DirectX::WIC_FLAGS_NONE, &meta, image);
        }

        if (FAILED(hr))
        {
            LOG_ERROR("TextureImporter: decode failed for '%s' (hr=0x%08X)",
                      sourcePath.c_str(), static_cast<unsigned>(hr));
            return {};
        }

        TexRole role = DetectRole(sourcePath, ext);

        // Treat float-format decoded images as HDR regardless of extension.
        if (meta.format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
            meta.format == DXGI_FORMAT_R32G32B32_FLOAT     ||
            meta.format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            role = TexRole::HDR;
        }

        // Ensure the in-memory format tag matches the intended colour space
        // WITHOUT triggering any pixel value conversion:
        //
        // - Color textures: stamp sRGB tag (pixel values ARE sRGB from PNG/JPG).
        //   OverrideFormat only changes the metadata, no gamma math applied.
        //   This guarantees BC7_UNORM_SRGB compression and GPU sampling both
        //   treat the data consistently.
        //
        // - Non-colour textures: strip sRGB tag so values stay raw/linear.
        if (role == TexRole::Color && !DirectX::IsSRGB(meta.format))
        {
            const DXGI_FORMAT srgbFmt = DirectX::MakeSRGB(meta.format);
            if (srgbFmt != meta.format && image.OverrideFormat(srgbFmt))
            {
                meta = image.GetMetadata();
                LOG_INFO("TextureImporter: stamped sRGB tag for color texture '%s'",
                         sourcePath.c_str());
            }
        }
        else if (role != TexRole::Color && role != TexRole::HDR &&
                 DirectX::IsSRGB(meta.format))
        {
            const DXGI_FORMAT linearFmt = DirectX::MakeLinear(meta.format);
            if (linearFmt != meta.format && image.OverrideFormat(linearFmt))
            {
                meta = image.GetMetadata();
                LOG_INFO("TextureImporter: stripped sRGB tag for linear-data texture '%s'",
                         sourcePath.c_str());
            }
        }

        const bool isHDR       = (role == TexRole::HDR);
        const bool isRawHeight = (role == TexRole::RawHeight);
        const bool isRawLUT2   = (role == TexRole::RawLUT2);
        const bool isRawColor  = (role == TexRole::RawColor);
        const bool isRaw       = isRawHeight || isRawLUT2 || isRawColor;

        // --- Convert to intermediate format (only if uncompressed) -----------
        // BC6H requires float; BC4/BC5/BC7 require RGBA8_UNORM.
        // The Raw* roles target their FINAL format here directly so the BC
        // compression step below can be cleanly skipped.
        if (!DirectX::IsCompressed(meta.format))
        {
            DXGI_FORMAT intermediate;
            if (isRawHeight)
            {
                // Heightmap / displacement — single-channel 16-bit UNORM.
                // Linear quantisation across [0,1] preserves the full 65 536
                // levels of a 16-bit source PNG/EXR, which is what the CPU
                // collision path round-trips. Convert drops G/B/A.
                intermediate = DXGI_FORMAT_R16_UNORM;
            }
            else if (isRawLUT2)
            {
                // BRDF LUT / split-sum table — 2-channel half float.
                // Source PNG decodes to R8G8B8A8; Convert keeps R + G,
                // drops B + A, and zero-extends to 16-bit float per channel.
                intermediate = DXGI_FORMAT_R16G16_FLOAT;
            }
            else if (isRawColor)
            {
                // Color ramp / palette LUT — 4-channel linear, bit-exact.
                intermediate = DXGI_FORMAT_R8G8B8A8_UNORM;
            }
            else if (isHDR)
            {
                intermediate = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            else if (role == TexRole::Color)
            {
                // After the sRGB tag override above, Color textures are already
                // tagged R8G8B8A8_UNORM_SRGB; using an sRGB intermediate keeps
                // Convert as a no-op (no gamma math).
                intermediate = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            }
            else
            {
                intermediate = DXGI_FORMAT_R8G8B8A8_UNORM;
            }

            if (meta.format != intermediate)
            {
                DirectX::ScratchImage converted;
                hr = DirectX::Convert(image.GetImages(), image.GetImageCount(),
                                      image.GetMetadata(),
                                      intermediate,
                                      DirectX::TEX_FILTER_DEFAULT,
                                      DirectX::TEX_THRESHOLD_DEFAULT,
                                      converted);
                if (SUCCEEDED(hr))
                {
                    image = std::move(converted);
                    meta  = image.GetMetadata();
                }
                else
                {
                    LOG_WARNING("TextureImporter: format convert failed for '%s' (hr=0x%08X)",
                                sourcePath.c_str(), static_cast<unsigned>(hr));
                }
            }
        }

        // --- Generate mip chain if missing -----------------------------------
        const bool needsMips = (meta.mipLevels == 1)
                             && !DirectX::IsCompressed(meta.format)
                             && (meta.width > 1 || meta.height > 1);

        if (needsMips)
        {
            DirectX::ScratchImage mipped;
            hr = DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(),
                                           image.GetMetadata(),
                                           DirectX::TEX_FILTER_DEFAULT,
                                           0 /*full chain*/, mipped);
            if (SUCCEEDED(hr))
            {
                image = std::move(mipped);
                meta  = image.GetMetadata();
                LOG_INFO("TextureImporter: generated %u mips for '%s'",
                         static_cast<uint32_t>(meta.mipLevels), sourcePath.c_str());
            }
            else
            {
                LOG_WARNING("TextureImporter: GenerateMipMaps failed for '%s' (hr=0x%08X)",
                            sourcePath.c_str(), static_cast<unsigned>(hr));
            }
        }

        // --- BC compression --------------------------------------------------
        // Skipped when:
        //   * source is already block-compressed (e.g. imported .dds), or
        //   * role is one of the Raw* uncompressed roles.
        DXGI_FORMAT bcFormat = DXGI_FORMAT_UNKNOWN;
        if (!DirectX::IsCompressed(meta.format) && !isRaw)
        {
            switch (role)
            {
            case TexRole::HDR:           bcFormat = DXGI_FORMAT_BC6H_UF16;      break;
            // BC5: stores RG, shader reconstructs B = sqrt(1 - dot(rg,rg))
            case TexRole::Normal:        bcFormat = DXGI_FORMAT_BC5_UNORM;      break;
            case TexRole::SingleChannel: bcFormat = DXGI_FORMAT_BC4_UNORM;      break;
            case TexRole::Color:         bcFormat = DXGI_FORMAT_BC7_UNORM_SRGB; break;
            default:                     bcFormat = DXGI_FORMAT_BC7_UNORM;      break;
            }

            // GPU-accelerated via BCCompressor (falls back to CPU if unavailable).
            DirectX::ScratchImage compressed;
            hr = BCCompressor::Get().Compress(
                    image.GetImages(), image.GetImageCount(),
                    image.GetMetadata(),
                    bcFormat, compressed);
            if (SUCCEEDED(hr))
            {
                image = std::move(compressed);
                meta  = image.GetMetadata();
            }
            else
            {
                LOG_WARNING("TextureImporter: BC compress (fmt=0x%X) failed for '%s' (hr=0x%08X), keeping uncompressed",
                            static_cast<unsigned>(bcFormat),
                            sourcePath.c_str(), static_cast<unsigned>(hr));
                bcFormat = meta.format; // reflect actual format in log
            }
        }
        else
        {
            bcFormat = meta.format;
        }

        // --- Serialize final image to DDS blob -------------------------------
        DirectX::Blob ddsBlob;
        hr = DirectX::SaveToDDSMemory(image.GetImages(), image.GetImageCount(),
                                       image.GetMetadata(), DirectX::DDS_FLAGS_NONE, ddsBlob);
        if (FAILED(hr))
        {
            LOG_ERROR("TextureImporter: SaveToDDSMemory failed for '%s' (hr=0x%08X)",
                      sourcePath.c_str(), static_cast<unsigned>(hr));
            return {};
        }

        // --- Build .itex blob: [AssetHeader][TextureMetadata][DDS bytes] -----
        meta = image.GetMetadata();
        TextureMetadata texMeta{};
        texMeta.width     = static_cast<uint32_t>(meta.width);
        texMeta.height    = static_cast<uint32_t>(meta.height);
        texMeta.depth     = static_cast<uint32_t>(meta.depth);
        texMeta.mipLevels = static_cast<uint16_t>(meta.mipLevels);
        texMeta.format    = static_cast<uint16_t>(meta.format);
        texMeta.dimension = static_cast<uint8_t>(meta.dimension);

        AssetHeader header{};
        header.magic        = MAGIC_TEXTURE;
        header.version      = ASSET_VERSION;
        header.resourceType = static_cast<uint16_t>(ResourceType::Texture);
        header.metadataSize = sizeof(TextureMetadata);
        header.dataSize     = static_cast<uint32_t>(ddsBlob.GetBufferSize());
        header.flags        = 0;
        header.reserved     = 0;

        const size_t totalSize =
            sizeof(AssetHeader) + sizeof(TextureMetadata) + ddsBlob.GetBufferSize();
        std::vector<uint8_t> blob(totalSize);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header,  sizeof(AssetHeader));     dst += sizeof(AssetHeader);
        std::memcpy(dst, &texMeta, sizeof(TextureMetadata)); dst += sizeof(TextureMetadata);
        std::memcpy(dst, ddsBlob.GetBufferPointer(), ddsBlob.GetBufferSize());

        // Order MUST match the TexRole enum.
        static const char* kRoleStr[] = {
            "Color", "Normal", "SingleChannel", "HDR",
            "RawHeight", "RawLUT2", "RawColor", "Other"
        };
        LOG_INFO("TextureImporter: '%s' → %ux%u %u mips fmt=0x%X role=%s (%zu B)",
                 sourcePath.c_str(),
                 texMeta.width, texMeta.height, texMeta.mipLevels,
                 static_cast<unsigned>(bcFormat),
                 kRoleStr[static_cast<int>(role)],
                 totalSize);
        return blob;
    }
}
