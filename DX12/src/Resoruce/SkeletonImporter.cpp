
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/SkeletonImporter.h"
#include "System/Log.h"

#include <assimp/scene.h>
#include <assimp/anim.h>

#include <DirectXMath.h>
#include <algorithm>
#include <cstring>
#include <unordered_set>

using namespace DirectX;

namespace
{
    // Convert Assimp column-vector matrix -> DX row-vector matrix (transpose).
    XMFLOAT4X4 ToXM(const aiMatrix4x4& m)
    {
        XMFLOAT4X4 o;
        o._11 = m.a1; o._12 = m.b1; o._13 = m.c1; o._14 = m.d1;
        o._21 = m.a2; o._22 = m.b2; o._23 = m.c2; o._24 = m.d2;
        o._31 = m.a3; o._32 = m.b3; o._33 = m.c3; o._34 = m.d3;
        o._41 = m.a4; o._42 = m.b4; o._43 = m.c4; o._44 = m.d4;
        return o;
    }

    // Multiply two XMFLOAT4X4 (row-vector convention).
    XMFLOAT4X4 Mul(const XMFLOAT4X4& a, const XMFLOAT4X4& b)
    {
        XMFLOAT4X4 out;
        XMStoreFloat4x4(&out, XMMatrixMultiply(XMLoadFloat4x4(&a), XMLoadFloat4x4(&b)));
        return out;
    }

    // Invert an XMFLOAT4X4.
    XMFLOAT4X4 Inv(const XMFLOAT4X4& m)
    {
        XMFLOAT4X4 out;
        XMVECTOR det;
        XMStoreFloat4x4(&out, XMMatrixInverse(&det, XMLoadFloat4x4(&m)));
        return out;
    }

    // Identity 4x4
    XMFLOAT4X4 Identity()
    {
        XMFLOAT4X4 out;
        XMStoreFloat4x4(&out, XMMatrixIdentity());
        return out;
    }

    // Build the full model-space transform of a node (product of all ancestor transforms).
    XMFLOAT4X4 GetNodeGlobalTransform(const aiNode* node)
    {
        if (!node) return Identity();
        XMFLOAT4X4 local = ToXM(node->mTransformation);
        if (!node->mParent) return local;
        return Mul(GetNodeGlobalTransform(node->mParent), local);
    }

    // Find an aiNode by name, searching the whole scene tree.
    const aiNode* FindNode(const aiNode* root, const std::string& name)
    {
        if (!root) return nullptr;
        if (root->mName.C_Str() == name) return root;
        for (unsigned i = 0; i < root->mNumChildren; ++i)
        {
            const aiNode* found = FindNode(root->mChildren[i], name);
            if (found) return found;
        }
        return nullptr;
    }
}

namespace Resource
{
    // -----------------------------------------------------------------------
    // FNV-32 hash
    // -----------------------------------------------------------------------
    uint32_t SkeletonImporter::Fnv32(const char* s)
    {
        uint32_t hash = 2166136261u;
        while (*s)
        {
            hash ^= static_cast<uint8_t>(*s++);
            hash *= 16777619u;
        }
        return hash;
    }

    // -----------------------------------------------------------------------
    // WalkBoneHierarchy — DFS from a node, recording bone slots in root-first
    // order so parentIndex[i] < i is always satisfied.
    // -----------------------------------------------------------------------
    void SkeletonImporter::WalkBoneHierarchy(
        const aiNode*                                   node,
        int32_t                                         parentSlot,
        const std::unordered_map<std::string, bool>&    boneNodeSet,
        SkeletonAsset&                                  out,
        std::unordered_map<std::string, uint32_t>&      nameToSlot)
    {
        const std::string name = node->mName.C_Str();

        int32_t mySlot = parentSlot; // carry parent's slot if this node is not a bone

        if (boneNodeSet.count(name))
        {
            if (nameToSlot.count(name) == 0 && out.boneCount < SkeletonAsset::MAX_BONES)
            {
                const uint32_t slot = out.boneCount++;
                nameToSlot[name]    = slot;

                out.parentIndex[slot] = parentSlot;

                // Store bone name
                //std::strncpy(out.boneNames[slot], name.c_str(), sizeof(out.boneNames[0]) - 1);
				strncpy_s(out.boneNames[slot], sizeof(out.boneNames[0]), name.c_str(), _TRUNCATE);
                out.boneNames[slot][sizeof(out.boneNames[0]) - 1] = '\0';

                // Build rest pose local TRS from node transform
                out.restPoseLocal[slot] = ToXM(node->mTransformation);

                // bindPose = global transform of the bone node
                out.bindPose[slot] = GetNodeGlobalTransform(node);

                // inverseBindPose = inverse of the global transform
                out.inverseBindPose[slot] = Inv(out.bindPose[slot]);

                mySlot = static_cast<int32_t>(slot);
            }
        }

        for (unsigned i = 0; i < node->mNumChildren; ++i)
            WalkBoneHierarchy(node->mChildren[i], mySlot, boneNodeSet, out, nameToSlot);
    }

    // -----------------------------------------------------------------------
    // CollectBones
    // -----------------------------------------------------------------------
    bool SkeletonImporter::CollectBones(
        const aiScene*                             scene,
        SkeletonAsset&                             out,
        std::unordered_map<std::string, uint32_t>& nameToSlot)
    {
        // Collect the set of all bone names referenced by any mesh.
        std::unordered_map<std::string, bool> boneNodeSet;
        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi)
                boneNodeSet[mesh->mBones[bi]->mName.C_Str()] = true;
        }

        if (boneNodeSet.empty())
            return false;


        out.boneCount = 0;

        // DFS from scene root — bones will be stored in root-first order,
        // which guarantees parentIndex[i] < i.
        WalkBoneHierarchy(scene->mRootNode, -1, boneNodeSet, out, nameToSlot);

        // Override inverseBindPose with the matrix stored in aiBone::mOffsetMatrix
        // (more accurate than inverting the global transform, because Assimp may
        // have applied post-processing that shifts the geometry).
        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi)
            {
                const aiBone* bone = mesh->mBones[bi];
                const std::string name = bone->mName.C_Str();
                auto it = nameToSlot.find(name);
                if (it == nameToSlot.end()) continue;
                const uint32_t slot = it->second;
                // mOffsetMatrix is the inverse bind pose in column-major Assimp convention.
                out.inverseBindPose[slot] = ToXM(bone->mOffsetMatrix);
                // Recompute bindPose as inverse of inverseBindPose for consistency.
                out.bindPose[slot] = Inv(out.inverseBindPose[slot]);
            }
        }

        // Populate nameToIndex hash map (FNV-32 keys).
        for (uint32_t i = 0; i < out.boneCount; ++i)
            out.nameToIndex[Fnv32(out.boneNames[i])] = i;

        return out.boneCount > 0;
    }

    // -----------------------------------------------------------------------
    // BuildBlendData — per-mesh BlendVertex arrays, top-4 influences per vertex.
    // -----------------------------------------------------------------------
    void SkeletonImporter::BuildBlendData(
        const aiScene*                                scene,
        const std::unordered_map<std::string, uint32_t>& nameToSlot,
        std::vector<std::vector<BlendVertex>>&            outPerMesh)
    {
        outPerMesh.resize(scene->mNumMeshes);

        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            if (mesh->mNumBones == 0) continue;

            const uint32_t nv = mesh->mNumVertices;
            outPerMesh[mi].assign(nv, BlendVertex{});

            // Accumulate up to 4 influences per vertex.
            // Use 8 floats per vertex temporarily (4 indices + 4 weights) then pack.
            struct RawInfluence { uint32_t boneIdx; float weight; };
            std::vector<std::vector<RawInfluence>> raw(nv);

            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi)
            {
                const aiBone* bone = mesh->mBones[bi];
                auto it = nameToSlot.find(bone->mName.C_Str());
                if (it == nameToSlot.end()) continue;
                const uint32_t boneSlot = it->second;

                for (unsigned wi = 0; wi < bone->mNumWeights; ++wi)
                {
                    const unsigned vi = bone->mWeights[wi].mVertexId;
                    if (vi >= nv) continue;
                    raw[vi].push_back({ boneSlot, bone->mWeights[wi].mWeight });
                }
            }

            for (uint32_t vi = 0; vi < nv; ++vi)
            {
                auto& inf = raw[vi];

                // Keep top-8 by weight.
                if (inf.size() > MAX_BONES_PER_VERTEX)
                {
                    std::partial_sort(inf.begin(), inf.begin() + MAX_BONES_PER_VERTEX, inf.end(),
                        [](const RawInfluence& a, const RawInfluence& b) {
                            return a.weight > b.weight;
                        });
                    inf.resize(MAX_BONES_PER_VERTEX);
                }

                // Normalise weights.
                float sum = 0.f;
                for (auto& r : inf) sum += r.weight;
                if (sum > 1e-6f)
                    for (auto& r : inf) r.weight /= sum;

                BlendVertex& bv = outPerMesh[mi][vi];
                for (uint32_t k = 0; k < MAX_BONES_PER_VERTEX; ++k)
                {
                    if (k < static_cast<uint32_t>(inf.size()))
                    {
                        const uint32_t idx = std::min(inf[k].boneIdx, 65535u);
                        bv.boneIndices[k] = static_cast<uint16_t>(idx);
                        bv.boneWeights[k] = static_cast<uint8_t>(std::min(inf[k].weight * 255.f + 0.5f, 255.f));
                    }
                    // else: remains 0 (no influence, weight=0)
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // BuildClip — convert one aiAnimation to a ClipAsset SOA.
    // -----------------------------------------------------------------------
    ClipAsset SkeletonImporter::BuildClip(
        const aiAnimation*                              anim,
        const std::unordered_map<std::string, uint32_t>& nameToSlot,
        uint32_t                                        boneCount,
        const SkeletonAsset&                            skeleton)
    {
        ClipAsset clip;
        clip.boneCount = boneCount;

        const double ticksPerSec = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 30.0;
        const double durationSec = anim->mDuration / ticksPerSec;

        // Sample at a fixed 30 fps.
        constexpr float kFrameRate = 30.f;
        const uint32_t frameCount = std::max(2u, static_cast<uint32_t>(durationSec * kFrameRate) + 1);

        clip.frameCount = frameCount;
        clip.duration   = static_cast<float>(durationSec);
        clip.frameRate  = kFrameRate;

        // Allocate SOA arrays and initialise every bone to its rest-pose local TRS.
        // Bones with no animation channel keep their rest pose every frame, which
        // is required for correct hierarchy: un-animated bones must stay at their
        // bind-pose offset from their parent, not collapse to the parent's origin.
        clip.positions.resize(static_cast<size_t>(boneCount) * frameCount);
        clip.rotations.resize(static_cast<size_t>(boneCount) * frameCount);
        clip.scales.resize   (static_cast<size_t>(boneCount) * frameCount);

        for (uint32_t bi = 0; bi < boneCount; ++bi)
        {
            XMFLOAT3 restPos = { 0.f, 0.f, 0.f };
            XMFLOAT4 restRot = { 0.f, 0.f, 0.f, 1.f };
            XMFLOAT3 restScl = { 1.f, 1.f, 1.f };

            XMVECTOR sc, rot, tr;
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
                clip.positions[idx] = restPos;
                clip.rotations[idx] = restRot;
                clip.scales[idx]    = restScl;
            }
        }

        for (unsigned ci = 0; ci < anim->mNumChannels; ++ci)
        {
            const aiNodeAnim* ch = anim->mChannels[ci];
            const std::string name = ch->mNodeName.C_Str();
            auto it = nameToSlot.find(name);
            if (it == nameToSlot.end()) continue;
            const uint32_t boneIdx = it->second;

            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const double t = (static_cast<double>(fi) / (frameCount - 1)) * anim->mDuration;

                // --- Sample position ---
                auto samplePos = [&]() -> XMFLOAT3
                {
                    if (ch->mNumPositionKeys == 0) return { 0.f, 0.f, 0.f };
                    uint32_t k = 0;
                    for (; k + 1 < ch->mNumPositionKeys; ++k)
                        if (t < ch->mPositionKeys[k + 1].mTime) break;
                    if (k + 1 >= ch->mNumPositionKeys)
                    {
                        auto& v = ch->mPositionKeys[k].mValue;
                        return { v.x, v.y, v.z };
                    }
                    const double t0 = ch->mPositionKeys[k].mTime;
                    const double t1 = ch->mPositionKeys[k + 1].mTime;
                    const float  f  = (t1 > t0) ? static_cast<float>((t - t0) / (t1 - t0)) : 0.f;
                    auto& a = ch->mPositionKeys[k].mValue;
                    auto& b = ch->mPositionKeys[k + 1].mValue;
                    return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
                };

                // --- Sample rotation ---
                auto sampleRot = [&]() -> XMFLOAT4
                {
                    if (ch->mNumRotationKeys == 0) return { 0.f, 0.f, 0.f, 1.f };
                    uint32_t k = 0;
                    for (; k + 1 < ch->mNumRotationKeys; ++k)
                        if (t < ch->mRotationKeys[k + 1].mTime) break;
                    if (k + 1 >= ch->mNumRotationKeys)
                    {
                        auto& q = ch->mRotationKeys[k].mValue;
                        return { q.x, q.y, q.z, q.w };
                    }
                    const double t0 = ch->mRotationKeys[k].mTime;
                    const double t1 = ch->mRotationKeys[k + 1].mTime;
                    const float  f  = (t1 > t0) ? static_cast<float>((t - t0) / (t1 - t0)) : 0.f;
                    auto& a = ch->mRotationKeys[k].mValue;
                    auto& b = ch->mRotationKeys[k + 1].mValue;
                    aiQuaternion out;
                    aiQuaternion::Interpolate(out, a, b, f);
                    return { out.x, out.y, out.z, out.w };
                };

                // --- Sample scale ---
                auto sampleScl = [&]() -> XMFLOAT3
                {
                    if (ch->mNumScalingKeys == 0) return { 1.f, 1.f, 1.f };
                    uint32_t k = 0;
                    for (; k + 1 < ch->mNumScalingKeys; ++k)
                        if (t < ch->mScalingKeys[k + 1].mTime) break;
                    if (k + 1 >= ch->mNumScalingKeys)
                    {
                        auto& v = ch->mScalingKeys[k].mValue;
                        return { v.x, v.y, v.z };
                    }
                    const double t0 = ch->mScalingKeys[k].mTime;
                    const double t1 = ch->mScalingKeys[k + 1].mTime;
                    const float  f  = (t1 > t0) ? static_cast<float>((t - t0) / (t1 - t0)) : 0.f;
                    auto& a = ch->mScalingKeys[k].mValue;
                    auto& b = ch->mScalingKeys[k + 1].mValue;
                    return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
                };

                const size_t idx = static_cast<size_t>(boneIdx) * frameCount + fi;
                clip.positions[idx] = samplePos();
                clip.rotations[idx] = sampleRot();
                clip.scales[idx]    = sampleScl();
            }
        }

        return clip;
    }

    // -----------------------------------------------------------------------
    // BuildGrantTable — detect PMX grant (付与) relationships by naming convention.
    //
    // Standard MMD D-bone pattern:
    //   "左足D"   → copies rotation from "左足"
    //   "左ひざD" → copies rotation from "左ひざ"
    //   etc.
    //
    // The "D" suffix in UTF-8 is a single ASCII byte (0x44), so we can check
    // if the bone name ends with "D" (after the Japanese characters).
    // -----------------------------------------------------------------------
    void SkeletonImporter::BuildGrantTable(SkeletonAsset& skel)
    {
        // Initialise all to -1 (no grant)
        for (uint32_t i = 0; i < SkeletonAsset::MAX_BONES; ++i)
        {
            skel.grantSource[i] = -1;
            skel.grantRatio[i]  = 0.f;
        }

        uint32_t grantCount = 0;

        for (uint32_t i = 0; i < skel.boneCount; ++i)
        {
            const std::string name(skel.boneNames[i]);
            if (name.empty()) continue;

            // Check for "D" suffix: bone name ends with ASCII 'D',
            // and the char before it is a multibyte UTF-8 char (not ASCII).
            // This distinguishes MMD D-bones from bones that happen to end in 'D'.
            if (name.size() >= 2 && name.back() == 'D')
            {
                // The character before 'D' should be part of a multibyte UTF-8 sequence
                // (i.e., high bit set) — this confirms it's a Japanese bone name + 'D'.
                uint8_t prevByte = static_cast<uint8_t>(name[name.size() - 2]);
                if (prevByte >= 0x80)  // multibyte UTF-8
                {
                    std::string baseName = name.substr(0, name.size() - 1);
                    uint32_t baseHash = Fnv32(baseName.c_str());
                    auto it = skel.nameToIndex.find(baseHash);
                    if (it != skel.nameToIndex.end())
                    {
                        skel.grantSource[i] = static_cast<int32_t>(it->second);
                        skel.grantRatio[i]  = 1.0f;
                        ++grantCount;
                    }
                }
            }

            // Check for "先EX" suffix — these copy from the corresponding "先" bone
            // or from the D-bone chain above them (their hierarchy parent usually
            // handles this via the parent index, so we skip explicit grant for now).
        }

        if (grantCount > 0)
            LOG_INFO("SkeletonImporter: detected %u D-bone grant relationships", grantCount);
    }

    // -----------------------------------------------------------------------
    // Import — top-level entry point
    // -----------------------------------------------------------------------
    bool SkeletonImporter::Import(const aiScene* scene, SkeletonImportResult& result)
    {
        if (!scene) return false;

        std::unordered_map<std::string, uint32_t> nameToSlot;
        if (!CollectBones(scene, result.skeleton, nameToSlot))
            return false;

        result.hasBones = true;

        // Build grant table after nameToIndex is populated
        BuildGrantTable(result.skeleton);

        LOG_INFO("SkeletonImporter: found %u bones", result.skeleton.boneCount);

        // Per-mesh blend data
        BuildBlendData(scene, nameToSlot, result.perMeshBlendData);

        // Animation clips
        result.clips.reserve(scene->mNumAnimations);
        for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai)
        {
            ClipAsset clip = BuildClip(scene->mAnimations[ai], nameToSlot,
                                        result.skeleton.boneCount, result.skeleton);
            const char* clipName = scene->mAnimations[ai]->mName.C_Str();
            LOG_INFO("SkeletonImporter: clip '%s' — %u frames, %.2fs",
                     clipName[0] ? clipName : "(unnamed)", clip.frameCount, clip.duration);
            result.clips.push_back(std::move(clip));
        }

        // ---- Pre-compute per-bone rest AABBs (for frustum culling) -----------
        {
            auto& skel = result.skeleton;

            for (uint32_t bi = 0; bi < skel.boneCount; ++bi)
            {
                skel.boneRestAABBs[bi].localMin = {  1e30f,  1e30f,  1e30f };
                skel.boneRestAABBs[bi].localMax = { -1e30f, -1e30f, -1e30f };
            }

            // Iterate all meshes that have blend data.
            for (uint32_t mi = 0; mi < result.perMeshBlendData.size() && mi < scene->mNumMeshes; ++mi)
            {
                const auto& bd = result.perMeshBlendData[mi];
                if (bd.empty()) continue;
                const aiMesh* mesh = scene->mMeshes[mi];
                if (!mesh || !mesh->mVertices) continue;

                const uint32_t vc = (std::min)(static_cast<uint32_t>(bd.size()), mesh->mNumVertices);
                for (uint32_t vi = 0; vi < vc; ++vi)
                {
                    const BlendVertex& bv = bd[vi];
                    const aiVector3D& vPos = mesh->mVertices[vi];

                    for (int w = 0; w < 4; ++w)
                    {
                        if (bv.boneWeights[w] == 0) continue;
                        uint32_t bi = bv.boneIndices[w];
                        if (bi >= skel.boneCount) continue;

                        // Transform vertex to bone-local space via inverseBindPose.
                        XMVECTOR vWorld = XMVectorSet(vPos.x, vPos.y, vPos.z, 1.f);
                        XMMATRIX invBind = XMLoadFloat4x4(&skel.inverseBindPose[bi]);
                        XMVECTOR vLocal = XMVector3TransformCoord(vWorld, invBind);

                        XMFLOAT3 lp;
                        XMStoreFloat3(&lp, vLocal);

                        auto& aabb = skel.boneRestAABBs[bi];
                        aabb.localMin.x = (std::min)(aabb.localMin.x, lp.x);
                        aabb.localMin.y = (std::min)(aabb.localMin.y, lp.y);
                        aabb.localMin.z = (std::min)(aabb.localMin.z, lp.z);
                        aabb.localMax.x = (std::max)(aabb.localMax.x, lp.x);
                        aabb.localMax.y = (std::max)(aabb.localMax.y, lp.y);
                        aabb.localMax.z = (std::max)(aabb.localMax.z, lp.z);
                    }
                }
            }

            const float kPad = 0.02f;
            for (uint32_t bi = 0; bi < skel.boneCount; ++bi)
            {
                auto& aabb = skel.boneRestAABBs[bi];
                if (aabb.localMin.x > aabb.localMax.x)
                {
                    aabb.localMin = { -kPad, -kPad, -kPad };
                    aabb.localMax = {  kPad,  kPad,  kPad };
                }
                else
                {
                    aabb.localMin.x -= kPad; aabb.localMin.y -= kPad; aabb.localMin.z -= kPad;
                    aabb.localMax.x += kPad; aabb.localMax.y += kPad; aabb.localMax.z += kPad;
                }
            }
            skel.hasBoneAABBs = true;
            LOG_INFO("SkeletonImporter: computed %u bone rest AABBs (Assimp)", skel.boneCount);
        }

        return true;
    }
}
