#include "Resource/AnimationImporter.h"
#include "Resource/AnimationResource.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/anim.h>

#ifdef _DEBUG
#pragma comment(lib, "assimp-vc143-mtd.lib")
#else
#pragma comment(lib, "assimp-vc143-mt.lib")
#endif

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace Resource
{
    // -------------------------------------------------------------------------
    // Interpolation helpers — identical algorithm to SkeletonImporter::BuildClip.
    // -------------------------------------------------------------------------
    static DirectX::XMFLOAT3 SamplePos(const aiVectorKey* keys, unsigned count, double t)
    {
        if (count == 0) return { 0.f, 0.f, 0.f };
        if (count == 1) return { keys[0].mValue.x, keys[0].mValue.y, keys[0].mValue.z };

        unsigned hi = 0;
        while (hi < count - 1 && keys[hi + 1].mTime <= t) ++hi;
        if (hi >= count - 1) return { keys[count - 1].mValue.x, keys[count - 1].mValue.y, keys[count - 1].mValue.z };

        const unsigned lo = hi;
        hi = lo + 1;
        const double span = keys[hi].mTime - keys[lo].mTime;
        const float  f    = (span > 1e-9) ? static_cast<float>((t - keys[lo].mTime) / span) : 0.f;

        return {
            keys[lo].mValue.x + f * (keys[hi].mValue.x - keys[lo].mValue.x),
            keys[lo].mValue.y + f * (keys[hi].mValue.y - keys[lo].mValue.y),
            keys[lo].mValue.z + f * (keys[hi].mValue.z - keys[lo].mValue.z)
        };
    }

    static DirectX::XMFLOAT4 SampleRot(const aiQuatKey* keys, unsigned count, double t)
    {
        if (count == 0) return { 0.f, 0.f, 0.f, 1.f };
        if (count == 1) return { keys[0].mValue.x, keys[0].mValue.y, keys[0].mValue.z, keys[0].mValue.w };

        unsigned hi = 0;
        while (hi < count - 1 && keys[hi + 1].mTime <= t) ++hi;
        if (hi >= count - 1) return { keys[count - 1].mValue.x, keys[count - 1].mValue.y, keys[count - 1].mValue.z, keys[count - 1].mValue.w };

        const unsigned lo = hi;
        hi = lo + 1;
        const double span = keys[hi].mTime - keys[lo].mTime;
        const float  f    = (span > 1e-9) ? static_cast<float>((t - keys[lo].mTime) / span) : 0.f;

        aiQuaternion q;
        aiQuaternion::Interpolate(q, keys[lo].mValue, keys[hi].mValue, f);
        q.Normalize();
        return { q.x, q.y, q.z, q.w };
    }

    // -------------------------------------------------------------------------
    // Build one AnimClipData from an aiAnimation.
    // Channels are stored by bone name, not skeleton bone index, so this clip
    // can be retargeted to any compatible skeleton via BindToSkeleton().
    // -------------------------------------------------------------------------
    static AnimClipData BuildAnimClipData(const aiAnimation* anim)
    {
        AnimClipData clip;

        // Clip name
        const char* rawName = anim->mName.C_Str();
        strncpy_s(clip.name, rawName[0] ? rawName : "unnamed", 64);

        // Duration + frame rate
        // Assimp uses ticks; convert to seconds.
        const double ticksPerSec = (anim->mTicksPerSecond > 1e-9)
            ? anim->mTicksPerSecond : 25.0;
        const double durationSec = anim->mDuration / ticksPerSec;

        clip.frameRate  = 30.f;
        clip.duration   = static_cast<float>(durationSec);
        clip.frameCount = std::max(1u, static_cast<uint32_t>(
            std::ceil(durationSec * clip.frameRate)));

        clip.channels.resize(anim->mNumChannels);

        for (unsigned ci = 0; ci < anim->mNumChannels; ++ci)
        {
            const aiNodeAnim* ch = anim->mChannels[ci];
            AnimClipChannel& out = clip.channels[ci];

            strncpy_s(out.name, ch->mNodeName.C_Str(), 64);
            out.positions.resize(clip.frameCount);
            out.rotations.resize(clip.frameCount);
            out.scales.resize(clip.frameCount);

            for (uint32_t fi = 0; fi < clip.frameCount; ++fi)
            {
                const double t = (fi / static_cast<double>(clip.frameRate)) * ticksPerSec;
                out.positions[fi] = SamplePos(ch->mPositionKeys, ch->mNumPositionKeys, t);
                out.rotations[fi] = SampleRot(ch->mRotationKeys, ch->mNumRotationKeys, t);
                out.scales[fi]    = SamplePos(ch->mScalingKeys,  ch->mNumScalingKeys,  t);
            }
        }

        return clip;
    }

    // -------------------------------------------------------------------------
    // Serialize one AnimClipData into dst.
    // Returns number of bytes written.
    //
    // Per-clip layout:
    //   uint32 channelCount, frameCount
    //   float  duration, frameRate
    //   char   name[64]
    //   For each channel:
    //     char   boneName[64]
    //     float3 positions[frameCount]
    //     float4 rotations[frameCount]
    //     float3 scales[frameCount]
    //   uint32 eventCount
    //   For each event: float time; uint32 nameHash
    // -------------------------------------------------------------------------
    static size_t ClipPayloadSize(const AnimClipData& clip)
    {
        size_t sz = 0;
        sz += 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64; // header
        for (const auto& ch : clip.channels)
        {
            sz += 64; // boneName
            sz += clip.frameCount * (3 + 4 + 3) * sizeof(float);
        }
        sz += sizeof(uint32_t); // eventCount
        sz += clip.events.size() * (sizeof(float) + sizeof(uint32_t));
        return sz;
    }

    static void WriteClip(const AnimClipData& clip, uint8_t*& dst)
    {
        const uint32_t cc = static_cast<uint32_t>(clip.channels.size());
        const uint32_t fc = clip.frameCount;
        std::memcpy(dst, &cc,           sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &fc,           sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &clip.duration,  sizeof(float));  dst += sizeof(float);
        std::memcpy(dst, &clip.frameRate, sizeof(float));  dst += sizeof(float);
        std::memcpy(dst, clip.name,       64);             dst += 64;

        for (const AnimClipChannel& ch : clip.channels)
        {
            std::memcpy(dst, ch.name, 64); dst += 64;
            std::memcpy(dst, ch.positions.data(), fc * 3 * sizeof(float)); dst += fc * 3 * sizeof(float);
            std::memcpy(dst, ch.rotations.data(), fc * 4 * sizeof(float)); dst += fc * 4 * sizeof(float);
            std::memcpy(dst, ch.scales.data(),    fc * 3 * sizeof(float)); dst += fc * 3 * sizeof(float);
        }

        const uint32_t ec = static_cast<uint32_t>(clip.events.size());
        std::memcpy(dst, &ec, sizeof(uint32_t)); dst += sizeof(uint32_t);
        for (const ClipAsset::AnimEvent& ev : clip.events)
        {
            std::memcpy(dst, &ev.time,     sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, &ev.nameHash, sizeof(uint32_t)); dst += sizeof(uint32_t);
        }
    }

    // =========================================================================
    // Morph clip serialization helpers
    // Per-morph-clip layout:
    //   uint32  channelCount, frameCount; float duration, frameRate; char name[64]
    //   For each channel: char morphName[64]; float weights[frameCount]
    // =========================================================================
    static size_t MorphClipPayloadSize(const MorphClipData& clip)
    {
        size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64; // header
        sz += clip.channels.size() * (64 + static_cast<size_t>(clip.frameCount) * sizeof(float));
        return sz;
    }

    static void WriteMorphClip(const MorphClipData& clip, uint8_t*& dst)
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
            std::memcpy(dst, ch.weights.data(), fc * sizeof(float)); dst += fc * sizeof(float);
        }
    }

    // =========================================================================
    // Extract morph animation from one aiAnimation.
    // Returns an empty MorphClipData (channels.empty()) if no morph tracks exist.
    // =========================================================================
    static MorphClipData BuildMorphClipData(const aiAnimation* anim,
                                             const aiScene*     scene,
                                             float              frameRate)
    {
        MorphClipData clip;
        if (anim->mNumMorphMeshChannels == 0) return clip;

        const double ticksPerSec = (anim->mTicksPerSecond > 1e-9)
            ? anim->mTicksPerSecond : 25.0;
        const double durationSec = anim->mDuration / ticksPerSec;

        const char* rawName = anim->mName.C_Str();
        strncpy_s(clip.name, rawName[0] ? rawName : "unnamed", 64);
        clip.frameRate  = frameRate;
        clip.frameCount = std::max(1u, static_cast<uint32_t>(
            std::ceil(durationSec * frameRate)));
        clip.duration   = static_cast<float>(durationSec);

        for (unsigned mi = 0; mi < anim->mNumMorphMeshChannels; ++mi)
        {
            const aiMeshMorphAnim* mma = anim->mMorphMeshChannels[mi];
            if (!mma || mma->mNumKeys == 0) continue;

            // Try to find the mesh to get morph target names
            const aiMesh* mesh = nullptr;
            for (unsigned si = 0; si < scene->mNumMeshes; ++si)
            {
                if (std::strcmp(scene->mMeshes[si]->mName.C_Str(), mma->mName.C_Str()) == 0)
                {
                    mesh = scene->mMeshes[si];
                    break;
                }
            }

            // Build a channel per morph target index mentioned in the keys
            // Collect all unique morph target indices across all keys
            uint32_t maxTargetIdx = 0;
            for (unsigned ki = 0; ki < mma->mNumKeys; ++ki)
                for (unsigned ti = 0; ti < mma->mKeys[ki].mNumValuesAndWeights; ++ti)
                    if (mma->mKeys[ki].mValues[ti] > maxTargetIdx)
                        maxTargetIdx = mma->mKeys[ki].mValues[ti];

            const uint32_t numTargets = maxTargetIdx + 1;

            for (uint32_t ti = 0; ti < numTargets; ++ti)
            {
                MorphClipChannel ch;
                // Name: mesh morph target name if available, else "morph_N"
                if (mesh && ti < mesh->mNumAnimMeshes)
                {
                    const char* tname = mesh->mAnimMeshes[ti]->mName.C_Str();
                    strncpy_s(ch.name, (tname && tname[0]) ? tname : "morph", _TRUNCATE);
                }
                else
                {
                    char fallback[64];
                    std::snprintf(fallback, sizeof(fallback), "morph_%u", ti);
                    strncpy_s(ch.name, fallback, _TRUNCATE);
                }

                ch.weights.resize(clip.frameCount, 0.f);

                // Sample this target's weight track at each frame
                for (uint32_t fi = 0; fi < clip.frameCount; ++fi)
                {
                    const double t = (fi / static_cast<double>(frameRate)) * ticksPerSec;
                    // Find surrounding keyframes
                    unsigned lo = 0;
                    while (lo + 1 < mma->mNumKeys && mma->mKeys[lo + 1].mTime <= t) ++lo;
                    const unsigned hi = lo + 1;

                    // Extract weights for target ti from lo and hi keys
                    auto GetWeight = [&](const aiMeshMorphKey& key) -> float {
                        for (unsigned w = 0; w < key.mNumValuesAndWeights; ++w)
                            if (key.mValues[w] == ti)
                                return static_cast<float>(key.mWeights[w]);
                        return 0.f;
                    };

                    if (hi >= mma->mNumKeys)
                    {
                        ch.weights[fi] = GetWeight(mma->mKeys[lo]);
                    }
                    else
                    {
                        const double span = mma->mKeys[hi].mTime - mma->mKeys[lo].mTime;
                        const float  alpha = (span > 1e-9) ? static_cast<float>((t - mma->mKeys[lo].mTime) / span) : 0.f;
                        const float  w0 = GetWeight(mma->mKeys[lo]);
                        const float  w1 = GetWeight(mma->mKeys[hi]);
                        ch.weights[fi] = w0 + alpha * (w1 - w0);
                    }
                }

                clip.channels.push_back(std::move(ch));
            }
        }
        return clip;
    }

    // =========================================================================
    // AnimationImporter::Import
    // =========================================================================
    std::vector<uint8_t> AnimationImporter::Import(const std::string&          sourcePath,
                                                     const std::vector<uint8_t>& sourceData)
    {
        if (sourceData.empty())
        {
            LOG_ERROR("AnimationImporter: empty source data for '%s'", sourcePath.c_str());
            return {};
        }

        // --- Load with Assimp (animation data only — skip heavy mesh processing) ---
        std::string ext = std::filesystem::path(sourcePath).extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const char* hint = ext.empty() ? "" : ext.c_str() + 1;

        Assimp::Importer importer;
        constexpr unsigned int flags =
            aiProcess_ConvertToLeftHanded | 
            aiProcess_Triangulate |        
            aiProcess_LimitBoneWeights |    
            aiProcess_PopulateArmatureData |
            aiProcess_GenNormals;          
        // aiProcess_Triangulate is minimal; mesh data is not needed but safe to skip.
        const aiScene* scene = importer.ReadFileFromMemory(
            sourceData.data(), sourceData.size(), flags, hint);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        {
            LOG_ERROR("AnimationImporter: Assimp failed on '%s': %s",
                      sourcePath.c_str(), importer.GetErrorString());
            return {};
        }

        if (scene->mNumAnimations == 0)
        {
            LOG_WARNING("AnimationImporter: no animations in '%s'", sourcePath.c_str());
            return {};
        }

        LOG_INFO("AnimationImporter: '%s' — %u animation(s)",
                 sourcePath.c_str(), scene->mNumAnimations);

        // --- Extract bone clips ---
        std::vector<AnimClipData>  clips;
        std::vector<MorphClipData> morphClips;
        clips.reserve(scene->mNumAnimations);
        morphClips.reserve(scene->mNumAnimations);

        for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai)
        {
            AnimClipData clip = BuildAnimClipData(scene->mAnimations[ai]);
            LOG_INFO("AnimationImporter:   clip[%u] '%s' — %u frames, %.2fs, %u bone channels",
                     ai, clip.name, clip.frameCount, clip.duration,
                     static_cast<unsigned>(clip.channels.size()));
            clips.push_back(std::move(clip));

            // --- Extract morph clip for the same animation ---
            MorphClipData morphClip = BuildMorphClipData(
                scene->mAnimations[ai], scene, clips.back().frameRate);
            if (!morphClip.channels.empty())
            {
                LOG_INFO("AnimationImporter:   morph clip[%u] — %u targets",
                         ai, static_cast<unsigned>(morphClip.channels.size()));
                morphClips.push_back(std::move(morphClip));
            }
        }

        // --- Compute payload size ---
        size_t payloadSz = 0;
        for (const auto& c : clips)      payloadSz += ClipPayloadSize(c);
        for (const auto& m : morphClips) payloadSz += MorphClipPayloadSize(m);

        // --- Build blob ---
        AnimationMetadata meta{};
        meta.clipCount      = static_cast<uint32_t>(clips.size());
        meta.morphClipCount = static_cast<uint32_t>(morphClips.size());
        meta.flags          = 0;
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

        std::memcpy(dst, &header, sizeof(AssetHeader));         dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(AnimationMetadata));   dst += sizeof(AnimationMetadata);

        for (const AnimClipData& c : clips)
            WriteClip(c, dst);
        for (const MorphClipData& m : morphClips)
            WriteMorphClip(m, dst);

        LOG_INFO("AnimationImporter: done — %zu bone clips, %zu morph clips, %zu bytes",
                 clips.size(), morphClips.size(), totalSz);

        return blob;
    }

} // namespace Resource
