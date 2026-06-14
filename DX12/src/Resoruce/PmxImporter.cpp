#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/PmxImporter.h"
#include "Resource/AssetHeader.h"
#include "Resource/MaterialSerializer.h"
#include "ECS/Components.h"
#include "System/Log.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <DirectXMath.h>
#include <windows.h>   // WideCharToMultiByte

using namespace DirectX;

namespace Resource
{
    // =====================================================================
    // Internal helpers
    // =====================================================================

    // PMX header config parsed from the globals block.
    struct PmxConfig
    {
        uint8_t  encoding;          // 0 = UTF-16LE, 1 = UTF-8
        uint8_t  additionalUVCount; // 0-4
        uint8_t  vertexIndexSize;   // 1, 2, or 4
        uint8_t  textureIndexSize;
        uint8_t  materialIndexSize;
        uint8_t  boneIndexSize;
        uint8_t  morphIndexSize;
        uint8_t  rigidBodyIndexSize;
        float    version;
    };

    // Safe read helpers ===================================================
    template<typename T>
    static bool Read(const uint8_t*& p, const uint8_t* end, T& out)
    {
        if (p + sizeof(T) > end) return false;
        std::memcpy(&out, p, sizeof(T));
        p += sizeof(T);
        return true;
    }

    static bool Skip(const uint8_t*& p, const uint8_t* end, size_t bytes)
    {
        if (p + bytes > end) return false;
        p += bytes;
        return true;
    }

    // Read a variable-size index. PMX uses UNSIGNED storage with a max-value sentinel for -1.
    //   1 byte: 0-254 valid, 0xFF = -1
    //   2 byte: 0-65534 valid, 0xFFFF = -1
    //   4 byte: 0-2^31 valid, 0xFFFFFFFF = -1 (or use signed int32 directly)
    // Returns -1 for "no reference" sentinel, otherwise the unsigned index as int32.
    static int32_t ReadIndex(const uint8_t*& p, const uint8_t* end, uint8_t sz)
    {
        if (p + sz > end) { p = end; return -1; }
        int32_t v = -1;
        switch (sz)
        {
        case 1: { uint8_t  t; std::memcpy(&t, p, 1); v = (t == 0xFF) ? -1 : static_cast<int32_t>(t); } break;
        case 2: { uint16_t t; std::memcpy(&t, p, 2); v = (t == 0xFFFF) ? -1 : static_cast<int32_t>(t); } break;
        case 4: { int32_t  t; std::memcpy(&t, p, 4); v = t; } break; // 4-byte stays signed per PMX spec
        }
        p += sz;
        return v;
    }

    // Read a variable-size unsigned vertex index.
    static uint32_t ReadVertexIndex(const uint8_t*& p, const uint8_t* end, uint8_t sz)
    {
        if (p + sz > end) { p = end; return 0; }
        uint32_t v = 0;
        switch (sz)
        {
        case 1: { uint8_t  t; std::memcpy(&t, p, 1); v = t; } break;
        case 2: { uint16_t t; std::memcpy(&t, p, 2); v = t; } break;
        case 4: { uint32_t t; std::memcpy(&t, p, 4); v = t; } break;
        }
        p += sz;
        return v;
    }

    // Read a PMX text field: int32 byteLength + raw bytes → UTF-8 string.
    static std::string ReadText(const uint8_t*& p, const uint8_t* end, uint8_t encoding)
    {
        int32_t byteLen = 0;
        if (!Read(p, end, byteLen) || byteLen <= 0)
            return {};
        if (p + byteLen > end) { p = end; return {}; }

        std::string result;
        if (encoding == 1) // UTF-8
        {
            result.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(byteLen));
        }
        else // UTF-16LE → UTF-8
        {
            const int wLen = byteLen / 2;
            const wchar_t* wStr = reinterpret_cast<const wchar_t*>(p);
            int uLen = WideCharToMultiByte(CP_UTF8, 0, wStr, wLen, nullptr, 0, nullptr, nullptr);
            if (uLen > 0)
            {
                result.resize(static_cast<size_t>(uLen));
                WideCharToMultiByte(CP_UTF8, 0, wStr, wLen, result.data(), uLen, nullptr, nullptr);
            }
        }
        p += byteLen;
        return result;
    }

    // No axis negation: MMD and DX12 are both left-handed (X-right, Y-up).
    // Z direction differs (MMD: toward viewer, DX: into screen) but this is
    // handled by camera placement, not by mirroring model data.
    static XMFLOAT3 ConvertPos3(float x, float y, float z) { return { x, y, z }; }

    // No quaternion conversion needed (same handedness).
    static XMFLOAT4 ConvertQuat(float qx, float qy, float qz, float qw)
    {
        return { qx, qy, qz, qw };
    }

    // FNV-32 hash
    static uint32_t Fnv32(const char* s)
    {
        uint32_t h = 2166136261u;
        while (*s) { h ^= static_cast<uint8_t>(*s++); h *= 16777619u; }
        return h;
    }

    // =====================================================================
    // PmxImporter::Import
    // =====================================================================
    PmxImportResult PmxImporter::Import(const uint8_t* data, size_t size)
    {
        PmxImportResult result;
        const uint8_t* p   = data;
        const uint8_t* end = data + size;

        // ---- Header --------------------------------------------------------
        // Magic "PMX " (4 bytes)
        if (size < 4 || std::memcmp(p, "PMX ", 4) != 0)
        {
            LOG_ERROR("PmxImporter: invalid magic");
            return result;
        }
        p += 4;

        PmxConfig cfg{};
        if (!Read(p, end, cfg.version))                    return result;
        if (cfg.version < 2.0f || cfg.version > 2.2f)
        {
            LOG_ERROR("PmxImporter: unsupported version %.1f", cfg.version);
            return result;
        }

        uint8_t globalsCount = 0;
        if (!Read(p, end, globalsCount))                   return result;
        if (globalsCount < 8 || p + globalsCount > end)
        {
            LOG_ERROR("PmxImporter: bad globals count %u", globalsCount);
            return result;
        }
        cfg.encoding          = p[0];
        cfg.additionalUVCount = p[1];
        cfg.vertexIndexSize   = p[2];
        cfg.textureIndexSize  = p[3];
        cfg.materialIndexSize = p[4];
        cfg.boneIndexSize     = p[5];
        cfg.morphIndexSize    = p[6];
        cfg.rigidBodyIndexSize= p[7];
        p += globalsCount;

        LOG_INFO("PmxImporter: version=%.1f encoding=%u addlUV=%u "
                 "vertIdxSz=%u texIdxSz=%u matIdxSz=%u boneIdxSz=%u morphIdxSz=%u rbIdxSz=%u",
                 cfg.version, cfg.encoding, cfg.additionalUVCount,
                 cfg.vertexIndexSize, cfg.textureIndexSize, cfg.materialIndexSize,
                 cfg.boneIndexSize, cfg.morphIndexSize, cfg.rigidBodyIndexSize);

        // Model name / comments (skip)
        ReadText(p, end, cfg.encoding); // name JP
        ReadText(p, end, cfg.encoding); // name EN
        ReadText(p, end, cfg.encoding); // comment JP
        ReadText(p, end, cfg.encoding); // comment EN

        // ---- Vertices ------------------------------------------------------
        int32_t vertexCount = 0;
        if (!Read(p, end, vertexCount) || vertexCount <= 0)
        {
            LOG_ERROR("PmxImporter: bad vertex count");
            return result;
        }

        result.positions.resize(static_cast<size_t>(vertexCount));
        result.normals.resize(static_cast<size_t>(vertexCount));
        result.uvs.resize(static_cast<size_t>(vertexCount));
        result.blendData.resize(static_cast<size_t>(vertexCount));
        if (cfg.additionalUVCount > 0)
            result.uv1.resize(static_cast<size_t>(vertexCount));

        for (int32_t vi = 0; vi < vertexCount; ++vi)
        {
            float px, py, pz, nx, ny, nz, u, v;
            if (!Read(p, end, px)) return result;
            if (!Read(p, end, py)) return result;
            if (!Read(p, end, pz)) return result;
            if (!Read(p, end, nx)) return result;
            if (!Read(p, end, ny)) return result;
            if (!Read(p, end, nz)) return result;
            if (!Read(p, end, u))  return result;
            if (!Read(p, end, v))  return result;

            result.positions[vi] = ConvertPos3(px, py, pz);
            result.normals[vi]   = ConvertPos3(nx, ny, nz);
            result.uvs[vi]       = { u, v };

            // Additional UVs: preserve the FIRST set's .xy as uv1 (the common
            // case — lightmap / detail / toon offset); skip any beyond it. Each
            // additional UV is a float4 (16 bytes) in PMX.
            if (cfg.additionalUVCount > 0)
            {
                float au0x, au0y, au0z, au0w;
                Read(p, end, au0x); Read(p, end, au0y);
                Read(p, end, au0z); Read(p, end, au0w);
                if (!result.uv1.empty())
                    result.uv1[vi] = { au0x, au0y };
                if (cfg.additionalUVCount > 1)
                    Skip(p, end, static_cast<size_t>(cfg.additionalUVCount - 1) * 16);
            }

            // Weight type
            uint8_t weightType = 0;
            if (!Read(p, end, weightType)) return result;

            BlendVertex& bv = result.blendData[vi];
            std::memset(&bv, 0, sizeof(bv));

            switch (weightType)
            {
            case 0: // BDEF1
            {
                int32_t b0 = ReadIndex(p, end, cfg.boneIndexSize);
                bv.boneIndices[0] = static_cast<uint16_t>(std::clamp(b0, 0, 65535));
                bv.boneWeights[0] = 255;
                break;
            }
            case 1: // BDEF2
            {
                int32_t b0 = ReadIndex(p, end, cfg.boneIndexSize);
                int32_t b1 = ReadIndex(p, end, cfg.boneIndexSize);
                float w0; Read(p, end, w0);
                bv.boneIndices[0] = static_cast<uint16_t>(std::clamp(b0, 0, 65535));
                bv.boneIndices[1] = static_cast<uint16_t>(std::clamp(b1, 0, 65535));
                bv.boneWeights[0] = static_cast<uint8_t>(std::clamp(w0 * 255.f + 0.5f, 0.f, 255.f));
                bv.boneWeights[1] = static_cast<uint8_t>(255 - bv.boneWeights[0]);
                break;
            }
            case 2: // BDEF4
            {
                int32_t bi[4]; float bw[4];
                for (int k = 0; k < 4; ++k) bi[k] = ReadIndex(p, end, cfg.boneIndexSize);
                for (int k = 0; k < 4; ++k) Read(p, end, bw[k]);
                float sum = bw[0] + bw[1] + bw[2] + bw[3];
                if (sum < 1e-6f) sum = 1.f;
                for (int k = 0; k < 4; ++k)
                {
                    bv.boneIndices[k] = static_cast<uint16_t>(std::clamp(bi[k], 0, 65535));
                    bv.boneWeights[k] = static_cast<uint8_t>(std::clamp(bw[k] / sum * 255.f + 0.5f, 0.f, 255.f));
                }
                break;
            }
            case 3: // SDEF — treat as BDEF2
            {
                int32_t b0 = ReadIndex(p, end, cfg.boneIndexSize);
                int32_t b1 = ReadIndex(p, end, cfg.boneIndexSize);
                float w0; Read(p, end, w0);
                Skip(p, end, 36); // C(f3) + R0(f3) + R1(f3)
                bv.boneIndices[0] = static_cast<uint16_t>(std::clamp(b0, 0, 65535));
                bv.boneIndices[1] = static_cast<uint16_t>(std::clamp(b1, 0, 65535));
                bv.boneWeights[0] = static_cast<uint8_t>(std::clamp(w0 * 255.f + 0.5f, 0.f, 255.f));
                bv.boneWeights[1] = static_cast<uint8_t>(255 - bv.boneWeights[0]);
                break;
            }
            case 4: // QDEF — treat as BDEF4
            {
                int32_t bi[4]; float bw[4];
                for (int k = 0; k < 4; ++k) bi[k] = ReadIndex(p, end, cfg.boneIndexSize);
                for (int k = 0; k < 4; ++k) Read(p, end, bw[k]);
                float sum = bw[0] + bw[1] + bw[2] + bw[3];
                if (sum < 1e-6f) sum = 1.f;
                for (int k = 0; k < 4; ++k)
                {
                    bv.boneIndices[k] = static_cast<uint16_t>(std::clamp(bi[k], 0, 65535));
                    bv.boneWeights[k] = static_cast<uint8_t>(std::clamp(bw[k] / sum * 255.f + 0.5f, 0.f, 255.f));
                }
                break;
            }
            default:
                LOG_ERROR("PmxImporter: unknown weight type %u at vertex %d", weightType, vi);
                return result;
            }

            // Edge scale (skip)
            float edgeScale; Read(p, end, edgeScale);
        }

        // Debug: bone index distribution
        {
            uint32_t maxBoneIdx = 0;
            uint32_t zeroWeightCount = 0;
            uint32_t highBoneCount = 0;  // bone index >= 128
            for (int32_t vi = 0; vi < vertexCount; ++vi)
            {
                const BlendVertex& bv = result.blendData[vi];
                uint32_t wsum = 0;
                for (int k = 0; k < 4; ++k)
                {
                    if (bv.boneWeights[k] > 0)
                    {
                        if (bv.boneIndices[k] > maxBoneIdx) maxBoneIdx = bv.boneIndices[k];
                        if (bv.boneIndices[k] >= 128) ++highBoneCount;
                    }
                    wsum += bv.boneWeights[k];
                }
                if (wsum == 0) ++zeroWeightCount;
            }
            LOG_INFO("PmxImporter: %d vertices — maxBoneIdx=%u  highBone(>=128)=%u  zeroWeight=%u",
                     vertexCount, maxBoneIdx, highBoneCount, zeroWeightCount);
        }

        // ---- Faces ---------------------------------------------------------
        int32_t indexCount = 0;
        if (!Read(p, end, indexCount) || indexCount <= 0)
        {
            LOG_ERROR("PmxImporter: bad index count");
            return result;
        }
        result.indices.resize(static_cast<size_t>(indexCount));
        for (int32_t i = 0; i < indexCount; ++i)
            result.indices[i] = ReadVertexIndex(p, end, cfg.vertexIndexSize);

        // No winding reversal: MMD and DX12 share the same left-handed convention.

        LOG_INFO("PmxImporter: %d indices (%d triangles)", indexCount, indexCount / 3);

        // ---- Textures ------------------------------------------------------
        int32_t textureCount = 0;
        if (!Read(p, end, textureCount)) return result;
        result.textureFiles.resize(static_cast<size_t>(std::max(textureCount, 0)));
        for (int32_t i = 0; i < textureCount; ++i)
            result.textureFiles[i] = ReadText(p, end, cfg.encoding);

        LOG_INFO("PmxImporter: %d textures", textureCount);

        // ---- Materials -----------------------------------------------------
        int32_t materialCount = 0;
        if (!Read(p, end, materialCount)) return result;

        uint32_t indexOffset = 0;
        result.materials.resize(static_cast<size_t>(std::max(materialCount, 0)));
        for (int32_t mi = 0; mi < materialCount; ++mi)
        {
            PmxMaterial& mat = result.materials[mi];
            mat.nameJP = ReadText(p, end, cfg.encoding);
            mat.nameEN = ReadText(p, end, cfg.encoding);

            Read(p, end, mat.diffuse);
            Read(p, end, mat.specular);
            Read(p, end, mat.shininess);
            Read(p, end, mat.ambient);

            uint8_t drawFlags; Read(p, end, drawFlags);
            Skip(p, end, 16); // edge color (f4)
            float edgeSize; Read(p, end, edgeSize);

            mat.textureIndex       = ReadIndex(p, end, cfg.textureIndexSize);
            mat.sphereTextureIndex = ReadIndex(p, end, cfg.textureIndexSize);
            Read(p, end, mat.sphereMode);

            uint8_t toonFlag; Read(p, end, toonFlag);
            if (toonFlag == 0)
                mat.toonTextureIndex = ReadIndex(p, end, cfg.textureIndexSize);
            else
            {
                uint8_t builtinIdx; Read(p, end, builtinIdx);
                mat.toonTextureIndex = -1; // built-in toon
            }

            ReadText(p, end, cfg.encoding); // memo

            int32_t faceIndexCount = 0;
            Read(p, end, faceIndexCount);
            mat.indexStart = indexOffset;
            mat.indexCount = static_cast<uint32_t>(std::max(faceIndexCount, 0));
            indexOffset += mat.indexCount;
        }

        // Debug: per-material bone index stats
        for (int32_t mi = 0; mi < materialCount; ++mi)
        {
            const PmxMaterial& mat = result.materials[mi];
            uint32_t maxBI = 0;
            for (uint32_t ii = 0; ii < mat.indexCount; ++ii)
            {
                uint32_t vi = result.indices[mat.indexStart + ii];
                if (vi < static_cast<uint32_t>(vertexCount))
                {
                    const BlendVertex& bv = result.blendData[vi];
                    for (int k = 0; k < 4; ++k)
                        if (bv.boneWeights[k] > 0 && bv.boneIndices[k] > maxBI)
                            maxBI = bv.boneIndices[k];
                }
            }
            LOG_INFO("PmxImporter:   mat[%d] idxStart=%u idxCount=%u maxBoneIdx=%u tex=%d",
                     mi, mat.indexStart, mat.indexCount, maxBI, mat.textureIndex);
        }
        LOG_INFO("PmxImporter: %d materials", materialCount);

        // ---- Bones ---------------------------------------------------------
        int32_t boneCount = 0;
        if (!Read(p, end, boneCount) || boneCount <= 0)
        {
            LOG_ERROR("PmxImporter: bad bone count");
            return result;
        }
        if (static_cast<uint32_t>(boneCount) > SkeletonAsset::MAX_BONES)
        {
            LOG_ERROR("PmxImporter: bone count %d exceeds MAX_BONES=%u", boneCount, SkeletonAsset::MAX_BONES);
            return result;
        }

        SkeletonAsset& skel = result.skeleton;
        skel.boneCount = static_cast<uint32_t>(boneCount);

        // Initialize grant to -1
        for (uint32_t i = 0; i < SkeletonAsset::MAX_BONES; ++i)
        {
            skel.grantSource[i] = -1;
            skel.grantRatio[i]  = 0.f;
        }

        // Temporary: world-space bone positions (before matrix construction)
        std::vector<XMFLOAT3> boneWorldPos(static_cast<size_t>(boneCount));

        for (int32_t bi = 0; bi < boneCount; ++bi)
        {
            // Name
            std::string nameJP = ReadText(p, end, cfg.encoding);
            ReadText(p, end, cfg.encoding); // name EN

            strncpy_s(skel.boneNames[bi], sizeof(skel.boneNames[0]),
                      nameJP.c_str(), _TRUNCATE);
            skel.boneNames[bi][63] = '\0';

            // Position (world-space in PMX → X-negate)
            float bx, by, bz;
            Read(p, end, bx); Read(p, end, by); Read(p, end, bz);
            boneWorldPos[bi] = ConvertPos3(bx, by, bz);

            // Parent index
            skel.parentIndex[bi] = ReadIndex(p, end, cfg.boneIndexSize);

            // Transform order
            int32_t transOrder = 0;
            Read(p, end, transOrder);
            skel.transformOrder[bi] = transOrder;

            // Flags
            uint16_t flags = 0;
            Read(p, end, flags);

            // Tail position
            if (flags & 0x0001) // tail is bone index
                ReadIndex(p, end, cfg.boneIndexSize); // skip
            else
                Skip(p, end, 12); // tail offset (f3)

            // Grant (付与) — flag bit 8 (append rotate) or bit 9 (append translate)
            // Note: PMX spec uses bit 8 = inherit rotation, bit 9 = inherit translation
            if (flags & 0x0100 || flags & 0x0200)
            {
                int32_t grantParent = ReadIndex(p, end, cfg.boneIndexSize);
                float   grantRatio  = 0.f;
                Read(p, end, grantRatio);
                skel.grantSource[bi] = grantParent;
                skel.grantRatio[bi]  = grantRatio;
            }

            // Fixed axis
            if (flags & 0x0400)
                Skip(p, end, 12); // axis vector (f3)

            // Local axis
            if (flags & 0x0800)
                Skip(p, end, 24); // localX(f3) + localZ(f3)

            // External parent deform
            if (flags & 0x2000)
            {
                int32_t extKey; Read(p, end, extKey);
            }

            // IK
            if (flags & 0x0020)
            {
                SkeletonAsset::IKChain chain;
                chain.ikBoneIndex = static_cast<uint32_t>(bi);

                int32_t targetIdx = ReadIndex(p, end, cfg.boneIndexSize);
                chain.targetBoneIndex = static_cast<uint32_t>(std::max(targetIdx, 0));

                int32_t loopCount = 0;
                Read(p, end, loopCount);
                chain.loopCount = static_cast<uint32_t>(std::max(loopCount, 0));

                Read(p, end, chain.angleLimit);

                int32_t linkCount = 0;
                Read(p, end, linkCount);

                chain.links.resize(static_cast<size_t>(std::max(linkCount, 0)));
                for (int32_t li = 0; li < linkCount; ++li)
                {
                    auto& link = chain.links[li];
                    int32_t linkBone = ReadIndex(p, end, cfg.boneIndexSize);
                    link.boneIndex = static_cast<uint32_t>(std::max(linkBone, 0));

                    uint8_t hasLimit = 0;
                    Read(p, end, hasLimit);
                    link.hasAngleLimit = (hasLimit != 0);

                    if (link.hasAngleLimit)
                    {
                        float minX, minY, minZ, maxX, maxY, maxZ;
                        Read(p, end, minX); Read(p, end, minY); Read(p, end, minZ);
                        Read(p, end, maxX); Read(p, end, maxY); Read(p, end, maxZ);
                        // No coordinate conversion → angle limits used as-is from PMX.
                        link.minAngle = { minX, minY, minZ };
                        link.maxAngle = { maxX, maxY, maxZ };
                    }
                }

                skel.ikChains.push_back(std::move(chain));
            }
        }

        LOG_INFO("PmxImporter: %d bones, %zu IK chains", boneCount,
                 skel.ikChains.size());

        // Count grants
        {
            uint32_t gc = 0;
            for (int32_t bi = 0; bi < boneCount; ++bi)
                if (skel.grantSource[bi] >= 0) ++gc;
            if (gc > 0)
                LOG_INFO("PmxImporter: %u grant relationships", gc);
        }

        // ---- Verify topological invariant ----------------------------------
        // PMX convention: parentIndex[i] < i. Verify; if violated, log a warning.
        for (int32_t bi = 1; bi < boneCount; ++bi)
        {
            if (skel.parentIndex[bi] >= bi && skel.parentIndex[bi] >= 0)
            {
                LOG_WARNING("PmxImporter: bone %d (%s) has parent %d >= self — topological violation",
                            bi, skel.boneNames[bi], skel.parentIndex[bi]);
            }
        }

        // ---- Build bind pose / inverse bind pose / rest pose local ---------
        // PMX bones store world-space rest position (no rotation at rest).
        for (int32_t bi = 0; bi < boneCount; ++bi)
        {
            const XMFLOAT3& wp = boneWorldPos[bi];

            // bindPose = pure translation matrix at world position
            XMStoreFloat4x4(&skel.bindPose[bi],
                XMMatrixTranslation(wp.x, wp.y, wp.z));

            // inverseBindPose = inverse of bindPose
            XMStoreFloat4x4(&skel.inverseBindPose[bi],
                XMMatrixTranslation(-wp.x, -wp.y, -wp.z));

            // restPoseLocal = local offset from parent
            const int32_t parent = skel.parentIndex[bi];
            if (parent < 0 || parent >= boneCount)
            {
                // Root bone: local = world
                skel.restPoseLocal[bi] = skel.bindPose[bi];
            }
            else
            {
                const XMFLOAT3& pp = boneWorldPos[parent];
                float dx = wp.x - pp.x;
                float dy = wp.y - pp.y;
                float dz = wp.z - pp.z;
                XMStoreFloat4x4(&skel.restPoseLocal[bi],
                    XMMatrixTranslation(dx, dy, dz));
            }
        }

        // ---- Build nameToIndex hash map ------------------------------------
        for (uint32_t bi = 0; bi < skel.boneCount; ++bi)
            skel.nameToIndex[Fnv32(skel.boneNames[bi])] = bi;

        // ---- Morphs --------------------------------------------------------
        int32_t morphCount = 0;
        if (!Read(p, end, morphCount)) morphCount = 0;

        uint32_t vertexMorphCount = 0;
        for (int32_t mi = 0; mi < morphCount; ++mi)
        {
            std::string morphNameJP = ReadText(p, end, cfg.encoding);
            ReadText(p, end, cfg.encoding); // name EN

            uint8_t panel = 0, morphType = 0;
            Read(p, end, panel);
            Read(p, end, morphType);

            int32_t offsetCount = 0;
            Read(p, end, offsetCount);

            if (morphType == 1) // Vertex morph
            {
                PmxMorphTarget target;
                strncpy_s(target.name, sizeof(target.name), morphNameJP.c_str(), _TRUNCATE);
                target.vertexIndices.reserve(static_cast<size_t>(offsetCount));
                target.positionDeltas.reserve(static_cast<size_t>(offsetCount));

                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    uint32_t vi = ReadVertexIndex(p, end, cfg.vertexIndexSize);
                    float dx, dy, dz;
                    Read(p, end, dx); Read(p, end, dy); Read(p, end, dz);
                    target.vertexIndices.push_back(vi);
                    // X-negate the position delta (matching vertex position convention)
                    target.positionDeltas.push_back(ConvertPos3(dx, dy, dz));
                }

                result.morphTargets.push_back(std::move(target));
                ++vertexMorphCount;
            }
            else if (morphType == 0) // Group morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.morphIndexSize); // morph index
                    float ratio; Read(p, end, ratio);
                }
            }
            else if (morphType == 2) // Bone morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.boneIndexSize);
                    Skip(p, end, 12 + 16); // position(f3) + quaternion(f4)
                }
            }
            else if (morphType >= 3 && morphType <= 7) // UV morph (3=UV, 4-7=addlUV)
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadVertexIndex(p, end, cfg.vertexIndexSize);
                    Skip(p, end, 16); // float4 UV delta
                }
            }
            else if (morphType == 8) // Material morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.materialIndexSize);
                    Skip(p, end, 1 + 16 + 12 + 4 + 12 + 16 + 4 + 16 + 16 + 16);
                    // op(1) + diffuse(f4) + specular(f3) + shininess(f) + ambient(f3)
                    // + edgeColor(f4) + edgeSize(f) + texTint(f4) + sphTint(f4) + toonTint(f4)
                }
            }
            else if (morphType == 9) // Flip morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.morphIndexSize);
                    float ratio; Read(p, end, ratio);
                }
            }
            else if (morphType == 10) // Impulse morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.rigidBodyIndexSize);
                    Skip(p, end, 1 + 12 + 12); // local(1) + velocity(f3) + torque(f3)
                }
            }
            else
            {
                LOG_WARNING("PmxImporter: unknown morph type %u at morph %d, skipping %d offsets",
                            morphType, mi, offsetCount);
                // Can't safely skip unknown types
                break;
            }
        }

        LOG_INFO("PmxImporter: %d morphs total, %u vertex morphs extracted",
                 morphCount, vertexMorphCount);

        // Store morph target names in skeleton for runtime name-matching
        skel.morphTargetCount = std::min(static_cast<uint32_t>(result.morphTargets.size()),
                                          SkeletonAsset::MAX_MORPH_TARGETS);
        for (uint32_t mi = 0; mi < skel.morphTargetCount; ++mi)
            std::memcpy(skel.morphTargetNames[mi], result.morphTargets[mi].name, 64);

        // (skip display frames, rigid bodies, joints — they follow morphs)

        result.success = true;

        LOG_SUCCESS("PmxImporter: import complete — %d verts, %d tris, %d bones, %zu materials, %u morphs",
                    vertexCount, indexCount / 3, boneCount, result.materials.size(), vertexMorphCount);
        return result;
    }

    // =====================================================================
    // ImportFromFile
    // =====================================================================
    PmxImportResult PmxImporter::ImportFromFile(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
        {
            LOG_ERROR("PmxImporter: cannot open '%s'", path.c_str());
            return {};
        }
        const auto sz = f.tellg();
        if (sz <= 0) return {};
        f.seekg(0);
        std::vector<uint8_t> data(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(data.data()), sz);
        return Import(data.data(), data.size());
    }

    // =====================================================================
    // BuildBlobs — produce .imsh / .iskel / .iscn from PmxImportResult.
    // Output format is identical to SceneImporter so SceneInstanceLoader
    // can load them without modification.
    // =====================================================================

    // PackedVertex — 32-byte legacy layout for PMX exports (no tangent stream).
    // Anonymous namespace gives this struct internal linkage so it doesn't
    // collide with the 48-byte `Resource::PackedVertex` defined in
    // SceneImporter.cpp — those two share a name otherwise, which is an ODR
    // violation that lets the linker's ICF folding merge the wrong sizeof
    // into vector<Resource::PackedVertex> instantiations and overrun the
    // reserved buffer mid-import. (Surfaced by Sponza glb crash 2026-05-09.)
    namespace {
        struct PackedVertex
        {
            float px, py, pz;
            float nx, ny, nz;
            float u, v;
        };
        static_assert(sizeof(PackedVertex) == 32, "PackedVertex must be 32 bytes");
    } // anonymous

    std::vector<PmxExportedFile> PmxImporter::BuildBlobs(
        const PmxImportResult& pmx,
        const std::string&     stem)
    {
        std::vector<PmxExportedFile> files;
        if (!pmx.success) return files;

        const SkeletonAsset& sk = pmx.skeleton;
        const uint32_t boneCount = sk.boneCount;

        // ---- Build a single .meshlib + per-material blend data in one pass ----
        // PMX materials define contiguous triangle ranges over a shared vertex buffer.
        // Instead of emitting one .imsh per material (the old path), we pack every
        // material into one .meshlib with an Entry per material — matches the
        // Assimp-based SceneImporter output and lets the renderer upload one shared
        // VB/IB for the whole model.
        //
        // CRITICAL: blend data must be collected in the SAME pass / same vertex
        // order as the packed vertex buffer, or .iskel's per-mesh blend indices
        // won't line up with the mesh entry's vertex range.
        const std::string meshLibName = stem + ".meshlib";   // single file
        uint32_t meshEntryCount = 0;                          // final Entry[] size

        // PMX gains a 2nd UV set (uv1) when the model declares additional UVs;
        // it never carries tangents or per-vertex color. Derive the interleaved
        // layout from that single fact via the shared helper so the .meshlib
        // round-trips through MeshLibrary::Load and MeshManager unchanged.
        const bool pmxHasUV1 = !pmx.uv1.empty();
        const uint32_t libFlags = pmxHasUV1 ? MESHLIB_FLAG_HAS_UV1 : 0u;
        const MeshLibVertexLayout L = ComputeMeshLibVertexLayout(libFlags);

        std::vector<uint8_t>          libVertBytes;   // shared interleaved VB
        std::vector<uint32_t>         libIndices;     // shared IB
        std::vector<MeshLibraryEntry> libEntries;     // one per non-empty PMX material
        libEntries.reserve(pmx.materials.size());

        // Append one interleaved vertex (pos/normal/uv0 [+uv1]) to the shared VB.
        auto appendVert = [&](const XMFLOAT3& pos, const XMFLOAT3& nrm,
                              const XMFLOAT2& uv0, const XMFLOAT2* uv1ptr)
        {
            const size_t base = libVertBytes.size();
            libVertBytes.resize(base + L.stride, 0u);
            uint8_t* vp = libVertBytes.data() + base;
            auto wF = [](uint8_t* p, float f) { std::memcpy(p, &f, sizeof(float)); };
            wF(vp + L.posOffset + 0, pos.x); wF(vp + L.posOffset + 4, pos.y); wF(vp + L.posOffset + 8, pos.z);
            wF(vp + L.normalOffset + 0, nrm.x); wF(vp + L.normalOffset + 4, nrm.y); wF(vp + L.normalOffset + 8, nrm.z);
            wF(vp + L.uv0Offset + 0, uv0.x); wF(vp + L.uv0Offset + 4, uv0.y);
            if (L.uv1Offset != 0xFFFFFFFFu)
            {
                const XMFLOAT2 u1 = uv1ptr ? *uv1ptr : uv0;
                wF(vp + L.uv1Offset + 0, u1.x); wF(vp + L.uv1Offset + 4, u1.y);
            }
        };

        struct MeshBlendEntry { uint32_t meshIdx; std::vector<BlendVertex> data; };
        std::vector<MeshBlendEntry> meshBlends;
        std::vector<std::vector<uint32_t>> perMeshGlobalVerts; // global vertex indices per submesh

        uint32_t vOffset = 0;   // running base vertex in the shared VB

        for (size_t mi = 0; mi < pmx.materials.size(); ++mi)
        {
            const PmxMaterial& mat = pmx.materials[mi];
            if (mat.indexCount == 0) continue;

            // Gather unique vertices referenced by this material's triangles.
            // Use a deterministic ordered vector instead of unordered_map to guarantee
            // the vertex order is identical between mesh VB slice and .iskel blend data.
            std::vector<uint32_t> uniqueGlobalVerts; // ordered list of unique global vertex indices
            std::unordered_map<uint32_t, uint32_t> globalToLocal;
            std::vector<uint32_t> localIndices;
            localIndices.reserve(mat.indexCount);

            for (uint32_t ii = 0; ii < mat.indexCount; ++ii)
            {
                uint32_t gi = pmx.indices[mat.indexStart + ii];
                auto it = globalToLocal.find(gi);
                if (it == globalToLocal.end())
                {
                    uint32_t li = static_cast<uint32_t>(uniqueGlobalVerts.size());
                    globalToLocal[gi] = li;
                    uniqueGlobalVerts.push_back(gi);
                    localIndices.push_back(li);
                }
                else
                    localIndices.push_back(it->second);
            }

            const uint32_t nv      = static_cast<uint32_t>(uniqueGlobalVerts.size());
            const uint32_t iStart  = static_cast<uint32_t>(libIndices.size());
            const uint32_t vStart  = vOffset;

            // Pack vertices + collect blend data in the SAME deterministic order.
            // Compute per-mesh AABB while packing — one pass.
            float bmin[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
            float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            std::vector<BlendVertex> blends(nv);

            for (uint32_t li = 0; li < nv; ++li)
            {
                uint32_t gi = uniqueGlobalVerts[li];
                const auto& pos = pmx.positions[gi];
                const auto& nrm = pmx.normals[gi];
                const auto& uv  = pmx.uvs[gi];
                const XMFLOAT2* uv1p = pmxHasUV1 ? &pmx.uv1[gi] : nullptr;
                appendVert(pos, nrm, uv, uv1p);
                blends[li] = pmx.blendData[gi];

                if (pos.x < bmin[0]) bmin[0] = pos.x;
                if (pos.y < bmin[1]) bmin[1] = pos.y;
                if (pos.z < bmin[2]) bmin[2] = pos.z;
                if (pos.x > bmax[0]) bmax[0] = pos.x;
                if (pos.y > bmax[1]) bmax[1] = pos.y;
                if (pos.z > bmax[2]) bmax[2] = pos.z;
            }

            // Re-base local indices into the shared VB pool.
            for (uint32_t li : localIndices)
                libIndices.push_back(li + vOffset);

            MeshLibraryEntry entry{};
            entry.vertexStart        = vStart;
            entry.vertexCount        = nv;
            entry.indexStart         = iStart;
            entry.indexCount         = static_cast<uint32_t>(localIndices.size());
            entry.aabbMin[0]         = bmin[0]; entry.aabbMin[1] = bmin[1]; entry.aabbMin[2] = bmin[2];
            entry.aabbMax[0]         = bmax[0]; entry.aabbMax[1] = bmax[1]; entry.aabbMax[2] = bmax[2];
            entry.defaultMaterialIdx = 0xFFFFFFFFu;  // resolved via F-line `mat=` in .iscn
            entry.flags              = 0;
            libEntries.push_back(entry);

            // Save blend data keyed by mesh entry index (== libEntries.size()-1).
            meshBlends.push_back({ meshEntryCount, std::move(blends) });

            // Save vertex remap for morph delta generation
            perMeshGlobalVerts.push_back(std::move(uniqueGlobalVerts));

            vOffset += nv;
            ++meshEntryCount;
        }

        // Emit the single .meshlib blob.
        if (!libEntries.empty())
        {
            const uint32_t entryBytes = static_cast<uint32_t>(libEntries.size() * sizeof(MeshLibraryEntry));
            const uint32_t vbBytes    = static_cast<uint32_t>(libVertBytes.size());
            const uint32_t ibBytes    = static_cast<uint32_t>(libIndices.size() * sizeof(uint32_t));

            MeshLibraryMetadata libMeta{};
            libMeta.meshCount    = static_cast<uint32_t>(libEntries.size());
            libMeta.vertexCount  = static_cast<uint32_t>(libVertBytes.size() / L.stride);
            libMeta.indexCount   = static_cast<uint32_t>(libIndices.size());
            libMeta.vertexStride = static_cast<uint16_t>(L.stride);
            libMeta.indexStride  = 4;
            libMeta.flags        = libFlags;
            libMeta.reserved     = 0;

            AssetHeader libHdr{};
            libHdr.magic        = MAGIC_MESHLIB;
            libHdr.version      = ASSET_VERSION;
            libHdr.resourceType = static_cast<uint16_t>(ResourceType::MeshLibrary);
            libHdr.metadataSize = sizeof(MeshLibraryMetadata);
            libHdr.dataSize     = entryBytes + vbBytes + ibBytes;

            const size_t total = sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)
                               + entryBytes + vbBytes + ibBytes;
            std::vector<uint8_t> blob(total);
            uint8_t* dst = blob.data();
            std::memcpy(dst, &libHdr,          sizeof(AssetHeader));         dst += sizeof(AssetHeader);
            std::memcpy(dst, &libMeta,         sizeof(MeshLibraryMetadata)); dst += sizeof(MeshLibraryMetadata);
            std::memcpy(dst, libEntries.data(),  entryBytes);                 dst += entryBytes;
            std::memcpy(dst, libVertBytes.data(), vbBytes);                   dst += vbBytes;
            std::memcpy(dst, libIndices.data(),  ibBytes);

            files.push_back({ stem + "/" + meshLibName, std::move(blob) });
        }

        // ---- Build .iskel blob (with extended PMX data) ----
        {
            uint32_t meshBlendCount = meshEntryCount;   // one blend stream per MeshLibraryEntry

            // --- Compute payload size ---
            size_t payloadSz = sizeof(uint32_t); // meshBlendCount
            payloadSz += boneCount * sizeof(int32_t);          // parentIndex
            payloadSz += boneCount * 16 * sizeof(float) * 3;   // invBind + bind + restLocal
            payloadSz += boneCount * 64;                        // boneNames

            // clips (none for PMX)

            // per-mesh blend data
            for (const auto& mb : meshBlends)
                payloadSz += 2 * sizeof(uint32_t) + mb.data.size() * sizeof(BlendVertex);

            // Extended PMX data (ISKEL_FLAG_EXTENDED):
            //   grantSource[boneCount] (int32)
            //   grantRatio[boneCount]  (float)
            //   transformOrder[boneCount] (int32)
            //   ikChainCount (uint32)
            //   per chain: ikBoneIndex(u32), targetBoneIndex(u32), loopCount(u32),
            //              angleLimit(f32), linkCount(u32)
            //   per link:  boneIndex(u32), hasAngleLimit(u8),
            //              [if hasLimit: minAngle(f3) + maxAngle(f3)]
            payloadSz += boneCount * sizeof(int32_t);   // grantSource
            payloadSz += boneCount * sizeof(float);     // grantRatio
            payloadSz += boneCount * sizeof(int32_t);   // transformOrder
            payloadSz += sizeof(uint32_t);              // ikChainCount
            for (const auto& chain : sk.ikChains)
            {
                payloadSz += 5 * sizeof(uint32_t);     // ik/target/loop/angleLimit/linkCount (20B)
                for (const auto& link : chain.links)
                {
                    payloadSz += sizeof(uint32_t) + 1;  // boneIndex + hasAngleLimit
                    if (link.hasAngleLimit)
                        payloadSz += 6 * sizeof(float); // minAngle(f3) + maxAngle(f3)
                }
            }

            // boneRestAABBs
            payloadSz += boneCount * 6 * sizeof(float); // min3 + max3 per bone

            // Morph target names
            payloadSz += sizeof(uint32_t); // morphTargetCount
            payloadSz += sk.morphTargetCount * 64; // morphTargetNames

            // --- Build header + metadata ---
            SkeletonMetadata smeta{};
            smeta.boneCount   = boneCount;
            smeta.clipCount   = 0;
            smeta.reserved[0] = meshBlendCount;
            smeta.reserved[1] = ISKEL_FLAG_EXTENDED;

            AssetHeader shdr{};
            shdr.magic        = MAGIC_SKELETON;
            shdr.version      = ASSET_VERSION;
            shdr.resourceType = static_cast<uint16_t>(ResourceType::Skeleton);
            shdr.metadataSize = sizeof(SkeletonMetadata);
            shdr.dataSize     = static_cast<uint32_t>(payloadSz);

            std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(SkeletonMetadata) + payloadSz, 0);
            uint8_t* dst = blob.data();

            std::memcpy(dst, &shdr,  sizeof(AssetHeader));      dst += sizeof(AssetHeader);
            std::memcpy(dst, &smeta, sizeof(SkeletonMetadata)); dst += sizeof(SkeletonMetadata);

            // --- Standard payload ---
            std::memcpy(dst, &meshBlendCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.parentIndex[i], sizeof(int32_t)); dst += sizeof(int32_t); }
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.inverseBindPose[i], 64); dst += 64; }
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.bindPose[i], 64); dst += 64; }
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.restPoseLocal[i], 64); dst += 64; }
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, sk.boneNames[i], 64); dst += 64; }

            // boneRestAABBs
            for (uint32_t i = 0; i < boneCount; ++i)
            {
                std::memcpy(dst, &sk.boneRestAABBs[i].localMin, 3 * sizeof(float)); dst += 3 * sizeof(float);
                std::memcpy(dst, &sk.boneRestAABBs[i].localMax, 3 * sizeof(float)); dst += 3 * sizeof(float);
            }

            // clips (none)

            // per-mesh blend data
            for (const auto& mb : meshBlends)
            {
                uint32_t idx = mb.meshIdx;
                uint32_t vc  = static_cast<uint32_t>(mb.data.size());
                std::memcpy(dst, &idx, sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, &vc,  sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, mb.data.data(), vc * sizeof(BlendVertex));
                dst += vc * sizeof(BlendVertex);
            }

            // --- Extended PMX data ---
            // grantSource
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.grantSource[i], sizeof(int32_t)); dst += sizeof(int32_t); }
            // grantRatio
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.grantRatio[i], sizeof(float)); dst += sizeof(float); }
            // transformOrder
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.transformOrder[i], sizeof(int32_t)); dst += sizeof(int32_t); }

            // IK chains
            uint32_t ikCount = static_cast<uint32_t>(sk.ikChains.size());
            std::memcpy(dst, &ikCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

            for (const auto& chain : sk.ikChains)
            {
                std::memcpy(dst, &chain.ikBoneIndex,     sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, &chain.targetBoneIndex, sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, &chain.loopCount,       sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, &chain.angleLimit,      sizeof(float));    dst += sizeof(float);

                uint32_t linkCount = static_cast<uint32_t>(chain.links.size());
                std::memcpy(dst, &linkCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

                for (const auto& link : chain.links)
                {
                    std::memcpy(dst, &link.boneIndex, sizeof(uint32_t)); dst += sizeof(uint32_t);
                    uint8_t hasLim = link.hasAngleLimit ? 1 : 0;
                    *dst++ = hasLim;
                    if (link.hasAngleLimit)
                    {
                        std::memcpy(dst, &link.minAngle, sizeof(XMFLOAT3)); dst += sizeof(XMFLOAT3);
                        std::memcpy(dst, &link.maxAngle, sizeof(XMFLOAT3)); dst += sizeof(XMFLOAT3);
                    }
                }
            }

            // Morph target names (for runtime name-matching against MorphClipAsset)
            std::memcpy(dst, &sk.morphTargetCount, sizeof(uint32_t)); dst += sizeof(uint32_t);
            for (uint32_t i = 0; i < sk.morphTargetCount; ++i)
            { std::memcpy(dst, sk.morphTargetNames[i], 64); dst += 64; }

            std::string skelFileName = stem + ".iskel";
            files.push_back({ stem + "/" + skelFileName, std::move(blob) });
        }

        // ---- Build .imorph blobs (per-submesh morph target deltas) ----
        std::vector<std::string> morphFileNames; // per submesh .imorph filename (empty = no morphs)
        if (!pmx.morphTargets.empty())
        {
            const uint32_t globalVertCount = static_cast<uint32_t>(pmx.positions.size());

            // Build global vertex → morph index lookup for each morph target (sparse→dense)
            // For each morph target, build a map: globalVertIdx → deltaIdx
            for (size_t si = 0; si < perMeshGlobalVerts.size(); ++si)
            {
                const auto& globalVerts = perMeshGlobalVerts[si];
                const uint32_t nv = static_cast<uint32_t>(globalVerts.size());
                if (nv == 0) continue;

                // Check if any morph target affects vertices in this submesh
                const uint32_t morphCount = static_cast<uint32_t>(pmx.morphTargets.size());

                // Build dense delta array: morphCount * nv float3
                std::vector<XMFLOAT3> denseDeltas(static_cast<size_t>(morphCount) * nv, XMFLOAT3{0,0,0});

                // Build reverse map: globalVertIdx → localVertIdx for this submesh
                std::unordered_map<uint32_t, uint32_t> globalToLocal;
                for (uint32_t li = 0; li < nv; ++li)
                    globalToLocal[globalVerts[li]] = li;

                bool hasAny = false;
                for (uint32_t mi = 0; mi < morphCount; ++mi)
                {
                    const auto& target = pmx.morphTargets[mi];
                    for (size_t di = 0; di < target.vertexIndices.size(); ++di)
                    {
                        auto it = globalToLocal.find(target.vertexIndices[di]);
                        if (it != globalToLocal.end())
                        {
                            denseDeltas[static_cast<size_t>(mi) * nv + it->second] = target.positionDeltas[di];
                            hasAny = true;
                        }
                    }
                }

                if (!hasAny)
                {
                    morphFileNames.push_back(""); // no morphs for this submesh
                    continue;
                }

                // Build .imorph blob
                MorphTargetMetadata mMeta{};
                mMeta.morphTargetCount = morphCount;
                mMeta.vertexCount      = nv;
                mMeta.meshFileIndex    = static_cast<uint32_t>(si);

                // Payload: morphCount × { name[64] + float3 deltas[nv] }
                size_t payloadSz = 0;
                for (uint32_t mi = 0; mi < morphCount; ++mi)
                    payloadSz += 64 + static_cast<size_t>(nv) * sizeof(XMFLOAT3);

                AssetHeader mHdr{};
                mHdr.magic        = MAGIC_MORPH;
                mHdr.version      = ASSET_VERSION;
                mHdr.resourceType = static_cast<uint16_t>(ResourceType::Mesh); // reuse enum
                mHdr.metadataSize = sizeof(MorphTargetMetadata);
                mHdr.dataSize     = static_cast<uint32_t>(payloadSz);

                std::vector<uint8_t> mBlob(sizeof(AssetHeader) + sizeof(MorphTargetMetadata) + payloadSz, 0);
                uint8_t* dst = mBlob.data();
                std::memcpy(dst, &mHdr,  sizeof(AssetHeader));           dst += sizeof(AssetHeader);
                std::memcpy(dst, &mMeta, sizeof(MorphTargetMetadata));   dst += sizeof(MorphTargetMetadata);

                for (uint32_t mi = 0; mi < morphCount; ++mi)
                {
                    // Name (64 bytes)
                    char name[64] = {};
                    strncpy_s(name, sizeof(name), pmx.morphTargets[mi].name, _TRUNCATE);
                    std::memcpy(dst, name, 64); dst += 64;

                    // Dense deltas for this morph target
                    const XMFLOAT3* srcDeltas = &denseDeltas[static_cast<size_t>(mi) * nv];
                    std::memcpy(dst, srcDeltas, nv * sizeof(XMFLOAT3));
                    dst += nv * sizeof(XMFLOAT3);
                }

                std::string morphFileName = stem + "_m" + std::to_string(si) + ".imorph";
                morphFileNames.push_back(morphFileName);
                files.push_back({ stem + "/" + morphFileName, std::move(mBlob) });
            }

            LOG_INFO("PmxImporter: built %zu .imorph files (%zu morph targets)",
                     morphFileNames.size(), pmx.morphTargets.size());
        }

        // ---- Generate .imat files for each material ----
        // Creates default materials with diffuse color from PMX, named after the PMX material.
        std::vector<std::string> matFileNames;  // parallel to meshFileNames
        {
            std::unordered_set<std::string> usedNames;
            size_t meshIdx = 0;
            for (size_t mi = 0; mi < pmx.materials.size(); ++mi)
            {
                const PmxMaterial& pmat = pmx.materials[mi];
                if (pmat.indexCount == 0) continue;

                // Pick a unique name: prefer EN, fall back to JP, then index
                std::string rawName = pmat.nameEN.empty() ? pmat.nameJP : pmat.nameEN;
                if (rawName.empty()) rawName = "material_" + std::to_string(mi);

                // Sanitize: replace spaces and special chars with underscore
                std::string safeName;
                for (char c : rawName)
                {
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-')
                        safeName += c;
                    else if (c > 0) // skip null, handle multi-byte as underscore
                        safeName += '_';
                }
                if (safeName.empty()) safeName = "mat_" + std::to_string(mi);

                // Deduplicate
                std::string uniqueName = safeName;
                int suffix = 1;
                while (usedNames.count(uniqueName))
                    uniqueName = safeName + "_" + std::to_string(suffix++);
                usedNames.insert(uniqueName);

                // Default MaterialComponent — only name matters, user edits later
                MaterialComponent mc{};

                // Save as .imat blob
                std::string matFileName = uniqueName + ".imat";
                std::string matRelPath = stem + "/Materials/" + matFileName;

                // Use MaterialSerializer to create the .imat blob
                // For BuildBlobs we create the blob in-memory and write via the file list
                files.push_back({ matRelPath, Resource::BuildMaterialBlob(mc) });

                matFileNames.push_back("Materials/" + matFileName);
                ++meshIdx;
            }

            LOG_INFO("PmxImporter: generated %zu .imat files", matFileNames.size());
        }

        // ---- Build .iscn blob ----
        // New format (P1-P6 meshlib rewrite): all mesh F-lines reference the
        // single shared .meshlib, with `mesh=K` picking the MeshLibraryEntry.
        {
            std::ostringstream ss;
            ss << "# ISCN engine scene container (PMX)\n";
            ss << "S nodes=1 meshfiles=" << meshEntryCount
               << " source=" << stem << "\n";

            ss << "N idx=0 parent=-1"
               << " tx=0 ty=0 tz=0"
               << " qx=0 qy=0 qz=0 qw=1"
               << " sx=1 sy=1 sz=1"
               << " mc=" << meshEntryCount;
            for (uint32_t k = 0; k < meshEntryCount; ++k)
                ss << " mr" << k << "=" << k;
            ss << " name=" << stem << "\n";

            for (uint32_t i = 0; i < meshEntryCount; ++i)
            {
                ss << "F idx=" << i << " file=" << meshLibName
                   << " mesh=" << i;
                if (i < matFileNames.size())
                    ss << " mat=" << matFileNames[i];
                ss << "\n";
            }

            ss << "K file=" << stem << ".iskel\n";

            // M-lines: per-mesh morph target files
            for (size_t i = 0; i < morphFileNames.size(); ++i)
            {
                if (!morphFileNames[i].empty())
                    ss << "M idx=" << i << " file=" << morphFileNames[i] << "\n";
            }

            std::string text = ss.str();

            SceneMetadata scMeta{};
            scMeta.nodeCount     = 1;
            scMeta.meshFileCount = meshEntryCount;
            scMeta.textLength    = static_cast<uint32_t>(text.size());
            scMeta.reserved      = 0;

            AssetHeader scHdr{};
            scHdr.magic        = MAGIC_SCENE;
            scHdr.version      = ASSET_VERSION;
            scHdr.resourceType = static_cast<uint16_t>(ResourceType::Scene);
            scHdr.metadataSize = sizeof(SceneMetadata);
            scHdr.dataSize     = static_cast<uint32_t>(text.size());

            std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(SceneMetadata) + text.size());
            uint8_t* dst = blob.data();
            std::memcpy(dst, &scHdr,  sizeof(AssetHeader));     dst += sizeof(AssetHeader);
            std::memcpy(dst, &scMeta, sizeof(SceneMetadata));  dst += sizeof(SceneMetadata);
            std::memcpy(dst, text.data(), text.size());

            files.push_back({ stem + "/" + stem + ".iscn", std::move(blob) });
        }

        LOG_SUCCESS("PmxImporter::BuildBlobs: %zu files for '%s'", files.size(), stem.c_str());
        return files;
    }

}
