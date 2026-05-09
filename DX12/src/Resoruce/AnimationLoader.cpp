#include "Resource/AnimationLoader.h"
#include "Resource/AnimationResource.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace Resource
{
    // -------------------------------------------------------------------------
    // Read one MorphClipData from the payload pointer; advances src.
    // -------------------------------------------------------------------------
    static MorphClipData ReadMorphClip(const uint8_t*& src)
    {
        MorphClipData clip;

        uint32_t cc = 0, fc = 0;
        std::memcpy(&cc,             src, sizeof(uint32_t)); src += sizeof(uint32_t);
        std::memcpy(&fc,             src, sizeof(uint32_t)); src += sizeof(uint32_t);
        std::memcpy(&clip.duration,  src, sizeof(float));    src += sizeof(float);
        std::memcpy(&clip.frameRate, src, sizeof(float));    src += sizeof(float);
        std::memcpy(clip.name,       src, 64);               src += 64;

        clip.frameCount = fc;
        clip.channels.resize(cc);

        for (uint32_t ci = 0; ci < cc; ++ci)
        {
            MorphClipChannel& ch = clip.channels[ci];
            std::memcpy(ch.name, src, 64); src += 64;
            ch.weights.resize(fc);
            std::memcpy(ch.weights.data(), src, fc * sizeof(float)); src += fc * sizeof(float);
        }

        return clip;
    }

    // -------------------------------------------------------------------------
    // Read one AnimClipData from the payload pointer; advances src.
    // -------------------------------------------------------------------------
    static AnimClipData ReadClip(const uint8_t*& src)
    {
        AnimClipData clip;

        uint32_t cc = 0, fc = 0;
        std::memcpy(&cc,          src, sizeof(uint32_t)); src += sizeof(uint32_t);
        std::memcpy(&fc,          src, sizeof(uint32_t)); src += sizeof(uint32_t);
        std::memcpy(&clip.duration,  src, sizeof(float)); src += sizeof(float);
        std::memcpy(&clip.frameRate, src, sizeof(float)); src += sizeof(float);
        std::memcpy(clip.name, src, 64);                  src += 64;

        clip.frameCount = fc;
        clip.channels.resize(cc);

        for (uint32_t ci = 0; ci < cc; ++ci)
        {
            AnimClipChannel& ch = clip.channels[ci];
            std::memcpy(ch.name, src, 64); src += 64;
            ch.positions.resize(fc);
            ch.rotations.resize(fc);
            ch.scales.resize(fc);
            std::memcpy(ch.positions.data(), src, fc * 3 * sizeof(float)); src += fc * 3 * sizeof(float);
            std::memcpy(ch.rotations.data(), src, fc * 4 * sizeof(float)); src += fc * 4 * sizeof(float);
            std::memcpy(ch.scales.data(),    src, fc * 3 * sizeof(float)); src += fc * 3 * sizeof(float);
        }

        uint32_t ec = 0;
        std::memcpy(&ec, src, sizeof(uint32_t)); src += sizeof(uint32_t);
        clip.events.resize(ec);
        for (uint32_t ei = 0; ei < ec; ++ei)
        {
            std::memcpy(&clip.events[ei].time,     src, sizeof(float));    src += sizeof(float);
            std::memcpy(&clip.events[ei].nameHash, src, sizeof(uint32_t)); src += sizeof(uint32_t);
        }

        return clip;
    }

    // =========================================================================
    // AnimationLoader::Load
    // =========================================================================
    LoadResult AnimationLoader::Load(const std::string&          path,
                                      const std::vector<uint8_t>& data)
    {
        LoadResult result;

        if (!ValidateHeader(data.data(), data.size(), MAGIC_ANIMATION))
        {
            LOG_ERROR("AnimationLoader: bad .ianim header in '%s'", path.c_str());
            return result;
        }

        const AnimationMetadata* meta = GetMetadata<AnimationMetadata>(data.data());
        const uint32_t clipCount      = meta->clipCount;
        const uint32_t morphClipCount = meta->morphClipCount;
        const bool posOfs = (meta->flags & 0x1) != 0;
        const bool rotOfs = (meta->flags & 0x2) != 0;

        auto res = std::make_unique<AnimationResource>();
        res->clips.reserve(clipCount);
        res->morphClips.reserve(morphClipCount);

        const uint8_t* src = GetPayload(data.data());
        for (uint32_t ci = 0; ci < clipCount; ++ci)
        {
            AnimClipData clip = ReadClip(src);
            clip.positionsAreOffsets = posOfs;
            clip.rotationsAreOffsets = rotOfs;
            res->clips.push_back(std::move(clip));
        }
        for (uint32_t mi = 0; mi < morphClipCount; ++mi)
            res->morphClips.push_back(ReadMorphClip(src));

        LOG_INFO("AnimationLoader: loaded '%s' — %u bone clip(s), %u morph clip(s)",
                 path.c_str(), clipCount, morphClipCount);

        result.resource = std::move(res);
        // No GPU upload needed for CPU-only clip data.
        return result;
    }

} // namespace Resource
