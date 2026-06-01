#include "Resource/AnimationSerializer.h"
#include "Resource/AnimationResource.h"
#include "Resource/AssetHeader.h"
#include "ECS/NotifySerialization.h"
#include "System/Log.h"

#include <cstring>
#include <string>
#include <vector>

namespace Resource
{
    namespace
    {
        // ---- Bone clip ------------------------------------------------------
        // Layout (matches AnimationImporter::WriteClip / AnimationLoader::ReadClip):
        //   uint32 channelCount, frameCount; float duration, frameRate; char name[64]
        //   per channel: char boneName[64]; float3[fc]; float4[fc]; float3[fc]
        //   uint32 eventCount; { float time; uint32 nameHash }[]
        size_t ClipPayloadSize(const AnimClipData& clip)
        {
            size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64;
            for (const auto& ch : clip.channels)
            {
                (void)ch;
                sz += 64;
                sz += static_cast<size_t>(clip.frameCount) * (3 + 4 + 3) * sizeof(float);
            }
            sz += sizeof(uint32_t);
            sz += clip.events.size() * (sizeof(float) + sizeof(uint32_t));
            return sz;
        }

        void WriteClip(const AnimClipData& clip, uint8_t*& dst)
        {
            const uint32_t cc = static_cast<uint32_t>(clip.channels.size());
            const uint32_t fc = clip.frameCount;
            std::memcpy(dst, &cc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &fc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &clip.duration,  sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, &clip.frameRate, sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, clip.name,       64);               dst += 64;

            for (const AnimClipChannel& ch : clip.channels)
            {
                std::memcpy(dst, ch.name, 64); dst += 64;
                // The importer guarantees positions/rotations/scales each have
                // frameCount entries; guard with the actual sizes to stay safe
                // if an editor produced a malformed channel.
                const size_t pBytes = static_cast<size_t>(fc) * 3 * sizeof(float);
                const size_t rBytes = static_cast<size_t>(fc) * 4 * sizeof(float);
                const size_t sBytes = static_cast<size_t>(fc) * 3 * sizeof(float);
                if (ch.positions.size() >= fc) std::memcpy(dst, ch.positions.data(), pBytes);
                dst += pBytes;
                if (ch.rotations.size() >= fc) std::memcpy(dst, ch.rotations.data(), rBytes);
                dst += rBytes;
                if (ch.scales.size() >= fc)    std::memcpy(dst, ch.scales.data(),    sBytes);
                dst += sBytes;
            }

            const uint32_t ec = static_cast<uint32_t>(clip.events.size());
            std::memcpy(dst, &ec, sizeof(uint32_t)); dst += sizeof(uint32_t);
            for (const ClipAsset::AnimEvent& ev : clip.events)
            {
                std::memcpy(dst, &ev.time,     sizeof(float));    dst += sizeof(float);
                std::memcpy(dst, &ev.nameHash, sizeof(uint32_t)); dst += sizeof(uint32_t);
            }
        }

        // ---- Morph clip -----------------------------------------------------
        size_t MorphClipPayloadSize(const MorphClipData& clip)
        {
            size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64;
            sz += clip.channels.size() *
                  (64 + static_cast<size_t>(clip.frameCount) * sizeof(float));
            return sz;
        }

        void WriteMorphClip(const MorphClipData& clip, uint8_t*& dst)
        {
            const uint32_t cc = static_cast<uint32_t>(clip.channels.size());
            const uint32_t fc = clip.frameCount;
            std::memcpy(dst, &cc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &fc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &clip.duration,  sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, &clip.frameRate, sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, clip.name,       64);               dst += 64;
            for (const MorphClipChannel& ch : clip.channels)
            {
                std::memcpy(dst, ch.name, 64); dst += 64;
                const size_t wBytes = static_cast<size_t>(fc) * sizeof(float);
                if (ch.weights.size() >= fc) std::memcpy(dst, ch.weights.data(), wBytes);
                dst += wBytes;
            }
        }
    } // namespace

    std::vector<uint8_t> SerializeAnimation(const AnimationResource& res)
    {
        if (res.clips.empty() && res.morphClips.empty())
            return {};

        // ---- Pre-encode the notify section (one JSON string per bone clip) ----
        // Only emit the section when at least one clip actually has notify
        // tracks — keeps notify-free .ianim files identical to importer output.
        bool anyNotifies = false;
        std::vector<std::string> notifyJson;
        notifyJson.reserve(res.clips.size());
        for (const AnimClipData& c : res.clips)
        {
            notifyJson.push_back(
                NotifyIO::TracksToJsonString(c.notifyTracks, c.duration, c.nextNotifyId));
            if (!c.notifyTracks.empty()) anyNotifies = true;
        }

        // ---- Derive offset flags from the per-clip flags (VMD round-trip) ----
        uint32_t flags = 0;
        for (const AnimClipData& c : res.clips)
        {
            if (c.positionsAreOffsets) flags |= ANIM_FLAG_POS_OFFSETS;
            if (c.rotationsAreOffsets) flags |= ANIM_FLAG_ROT_OFFSETS;
        }

        // ---- Compute payload size ----
        size_t payloadSz = 0;
        for (const auto& c : res.clips)      payloadSz += ClipPayloadSize(c);
        for (const auto& m : res.morphClips) payloadSz += MorphClipPayloadSize(m);

        size_t notifySectionSz = 0;
        if (anyNotifies)
        {
            flags |= ANIM_FLAG_HAS_NOTIFIES;
            notifySectionSz += sizeof(uint32_t); // notifyClipCount
            for (const std::string& s : notifyJson)
                notifySectionSz += sizeof(uint32_t) + s.size();
            payloadSz += notifySectionSz;
        }

        // ---- Build blob ----
        AnimationMetadata meta{};
        meta.clipCount      = static_cast<uint32_t>(res.clips.size());
        meta.morphClipCount = static_cast<uint32_t>(res.morphClips.size());
        meta.flags          = flags;
        meta.reserved       = 0;

        AssetHeader header{};
        header.magic        = MAGIC_ANIMATION;
        header.version      = ASSET_VERSION;
        header.resourceType = static_cast<uint16_t>(ResourceType::Animation);
        header.metadataSize = sizeof(AnimationMetadata);
        header.dataSize     = static_cast<uint32_t>(payloadSz);
        header.flags        = 0;
        header.reserved     = 0;

        const size_t totalSz = sizeof(AssetHeader) + sizeof(AnimationMetadata) + payloadSz;
        std::vector<uint8_t> blob(totalSz, 0);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header, sizeof(AssetHeader));       dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(AnimationMetadata)); dst += sizeof(AnimationMetadata);

        for (const AnimClipData& c : res.clips)      WriteClip(c, dst);
        for (const MorphClipData& m : res.morphClips) WriteMorphClip(m, dst);

        if (anyNotifies)
        {
            const uint32_t clipCount = static_cast<uint32_t>(notifyJson.size());
            std::memcpy(dst, &clipCount, sizeof(uint32_t)); dst += sizeof(uint32_t);
            for (const std::string& s : notifyJson)
            {
                const uint32_t len = static_cast<uint32_t>(s.size());
                std::memcpy(dst, &len, sizeof(uint32_t)); dst += sizeof(uint32_t);
                if (len) { std::memcpy(dst, s.data(), len); dst += len; }
            }
        }

        LOG_INFO("AnimationSerializer: wrote %zu bone clip(s), %zu morph clip(s), "
                 "notifies=%s, %zu bytes",
                 res.clips.size(), res.morphClips.size(),
                 anyNotifies ? "yes" : "no", totalSz);

        return blob;
    }
} // namespace Resource
