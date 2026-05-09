#pragma once

// AnimClipChannel  — one bone's keyframe track, identified by bone name.
// AnimClipData     — a full animation clip with named channels (not yet bound to
//                    a specific skeleton's bone index ordering).
// AnimationResource — collection of AnimClipData loaded from one .ianim file.
//
// Usage:
//   1. Load a .ianim via AnimationClipSystem::AcquireClip().
//   2. Bind to a skeleton:
//        ClipAsset clip = animResource.clips[i].BindToSkeleton(skeleton);
//   3. Register with ClipLibrary:
//        uint32_t idx = clipLib.Register(std::move(clip));
//   4. Assign to entity: AnimationComponent::primaryClip = idx.

#include "Resource/Resource.h"
#include "Resource/SkeletonAsset.h"  // ClipAsset, SkeletonAsset, AnimEvent

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <DirectXMath.h>

namespace Resource
{
    // -----------------------------------------------------------------------
    // One bone's full keyframe track.
    // frameCount == parent AnimClipData::frameCount.
    // -----------------------------------------------------------------------
    struct AnimClipChannel
    {
        char name[64] = {};  // bone name — used to map to a skeleton bone index

        // Parallel arrays; each has frameCount entries.
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT4> rotations;  // quaternion xyzw
        std::vector<DirectX::XMFLOAT3> scales;
    };

    // -----------------------------------------------------------------------
    // One animation clip with named channels.
    // Not yet bound to any skeleton — bone-index ordering is resolved by
    // BindToSkeleton().
    // -----------------------------------------------------------------------
    struct AnimClipData
    {
        char     name[64]   = {};   // clip name (e.g. "Walk", "Run_01")
        uint32_t frameCount = 0;
        float    duration   = 0.f;  // clip length in seconds
        float    frameRate  = 30.f;

        // VMD clips store position/rotation as OFFSETS from the rest pose, while
        // FBX/GLTF clips store ABSOLUTE local transforms.  When these flags are
        // true, BindToSkeleton will ADD positions and COMPOSE rotations with the
        // skeleton rest pose instead of overwriting them.
        bool positionsAreOffsets = false;
        bool rotationsAreOffsets = false;

        std::vector<AnimClipChannel>      channels;
        std::vector<ClipAsset::AnimEvent> events;

        // Produce a ClipAsset bound to the given skeleton.
        //
        // Each channel is matched to a skeleton bone by FNV-32 name hash.
        // Channels not present in the clip default to rest-pose
        // (identity position/scale, identity rotation).
        //
        // The returned ClipAsset has boneCount == skeleton.boneCount
        // and can be registered directly into ClipLibrary.
        ClipAsset BindToSkeleton(const SkeletonAsset& skeleton) const;
    };

    // -----------------------------------------------------------------------
    // One morph target's weight track (named, sparse-ready, dense on load).
    // frameCount == parent MorphClipData::frameCount.
    // -----------------------------------------------------------------------
    struct MorphClipChannel
    {
        char name[64] = {};          // morph target name — matched against mesh at bind time
        std::vector<float> weights;  // frameCount entries in [0, 1]
    };

    // -----------------------------------------------------------------------
    // One morph/face animation clip with named morph-target channels.
    // Not yet bound to any mesh — names resolved at runtime by AnimationSystem.
    // -----------------------------------------------------------------------
    struct MorphClipData
    {
        char     name[64]   = {};   // clip name (matches paired bone clip name)
        uint32_t frameCount = 0;
        float    duration   = 0.f;
        float    frameRate  = 30.f;
        std::vector<MorphClipChannel> channels;

        // Convert to a runtime SOA MorphClipAsset ready for MorphClipLibrary.
        MorphClipAsset ToRuntimeClip() const;
    };

    // -----------------------------------------------------------------------
    // AnimationResource — all clips from one .ianim source file.
    // Owned by ResourceManager; accessed via AnimHandle from AnimationClipSystem.
    // -----------------------------------------------------------------------
    class AnimationResource : public Resource
    {
    public:
        std::vector<AnimClipData>  clips;       // bone animation clips
        std::vector<MorphClipData> morphClips;  // morph/face animation clips
    };

    // -----------------------------------------------------------------------
    // Inline implementation of MorphClipData::ToRuntimeClip
    // -----------------------------------------------------------------------
    inline MorphClipAsset MorphClipData::ToRuntimeClip() const
    {
        MorphClipAsset out;
        out.morphCount = static_cast<uint32_t>(
            (std::min)(channels.size(),
                     static_cast<size_t>(MorphClipAsset::MAX_MORPHS)));
        out.frameCount = frameCount;
        out.duration   = duration;
        out.frameRate  = frameRate;

        out.weights.assign(static_cast<size_t>(out.morphCount) * frameCount, 0.f);

        for (uint32_t mi = 0; mi < out.morphCount; ++mi)
        {
            const MorphClipChannel& ch = channels[mi];
			strncpy_s(out.morphNames[mi], ch.name, 64);

            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const size_t idx = static_cast<size_t>(mi) * frameCount + fi;
                out.weights[idx] = (fi < static_cast<uint32_t>(ch.weights.size()))
                                   ? ch.weights[fi] : 0.f;
            }
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // Inline implementation of BindToSkeleton
    // -----------------------------------------------------------------------
    inline ClipAsset AnimClipData::BindToSkeleton(const SkeletonAsset& skeleton) const
    {
        using namespace DirectX;

        // FNV-32 helper (same as SkeletonImporter)
        auto Fnv32 = [](const char* s) -> uint32_t {
            uint32_t h = 2166136261u;
            for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
            return h;
        };

        ClipAsset out;
        out.boneCount  = skeleton.boneCount;
        out.frameCount = frameCount;
        out.duration   = duration;
        out.frameRate  = frameRate;
        out.events     = events;

        const size_t kf = static_cast<size_t>(out.boneCount) * out.frameCount;
        out.positions.resize(kf);
        out.rotations.resize(kf);
        out.scales   .resize(kf);

        // Default every bone to its rest-pose local TRS.
        // Bones without an animation channel stay at rest pose every frame.
        // This fixes clips (VMD, partial FBX) that only key a subset of bones —
        // without this, un-keyed bones default to zero-position and collapse to
        // their parent's origin, pulling attached mesh vertices to the root point.
        for (uint32_t bi = 0; bi < skeleton.boneCount; ++bi)
        {
            XMVECTOR sc, rot, tr;
            XMFLOAT3 restPos = { 0.f, 0.f, 0.f };
            XMFLOAT4 restRot = { 0.f, 0.f, 0.f, 1.f };
            XMFLOAT3 restScl = { 1.f, 1.f, 1.f };

            if (XMMatrixDecompose(&sc, &rot, &tr,
                                  XMLoadFloat4x4(&skeleton.restPoseLocal[bi])))
            {
                XMStoreFloat3(&restPos, tr);
                XMStoreFloat4(&restRot, rot);
                XMStoreFloat3(&restScl, sc);
            }

            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const size_t idx = static_cast<size_t>(bi) * frameCount + fi;
                out.positions[idx] = restPos;
                out.rotations[idx] = restRot;
                out.scales[idx]    = restScl;
            }
        }

        // Build channel name-hash → channel index map
        std::unordered_map<uint32_t, uint32_t> hashToChannel;
        hashToChannel.reserve(channels.size());
        for (uint32_t ci = 0; ci < static_cast<uint32_t>(channels.size()); ++ci)
            hashToChannel[Fnv32(channels[ci].name)] = ci;

        // Overwrite rest-pose defaults with actual keyframe data for animated bones.
        // When positionsAreOffsets / rotationsAreOffsets are set (VMD clips), the
        // channel values are OFFSETS from rest pose rather than absolute local TRS.
        for (uint32_t bi = 0; bi < skeleton.boneCount; ++bi)
        {
            const uint32_t h = Fnv32(skeleton.boneNames[bi]);
            auto it = hashToChannel.find(h);
            if (it == hashToChannel.end()) continue;

            // Decompose rest pose for offset composition
            XMFLOAT3 restPos = { 0.f, 0.f, 0.f };
            XMFLOAT4 restRot = { 0.f, 0.f, 0.f, 1.f };
            if (positionsAreOffsets || rotationsAreOffsets)
            {
                XMVECTOR sc, rot, tr;
                if (XMMatrixDecompose(&sc, &rot, &tr,
                                      XMLoadFloat4x4(&skeleton.restPoseLocal[bi])))
                {
                    XMStoreFloat3(&restPos, tr);
                    XMStoreFloat4(&restRot, rot);
                }
            }

            const AnimClipChannel& ch = channels[it->second];
            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const size_t idx = static_cast<size_t>(bi) * frameCount + fi;

                if (fi < static_cast<uint32_t>(ch.positions.size()))
                {
                    if (positionsAreOffsets)
                    {
                        out.positions[idx] = { restPos.x + ch.positions[fi].x,
                                               restPos.y + ch.positions[fi].y,
                                               restPos.z + ch.positions[fi].z };
                    }
                    else
                    {
                        out.positions[idx] = ch.positions[fi];
                    }
                }

                if (fi < static_cast<uint32_t>(ch.rotations.size()))
                {
                    if (rotationsAreOffsets)
                    {
                        XMVECTOR qRest = XMLoadFloat4(&restRot);
                        XMVECTOR qAnim = XMLoadFloat4(&ch.rotations[fi]);
                        XMStoreFloat4(&out.rotations[idx],
                                      XMQuaternionMultiply(qRest, qAnim));
                    }
                    else
                    {
                        out.rotations[idx] = ch.rotations[fi];
                    }
                }

                if (fi < static_cast<uint32_t>(ch.scales.size()))
                    out.scales[idx] = ch.scales[fi];
            }
        }

        return out;
    }
}
