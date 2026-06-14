#include "Resource/SceneImporter.h"
#include "Resource/AssetHeader.h"
#include "Resource/SkeletonImporter.h"
#include "Resource/SkeletonAsset.h"
#include "Resource/MaterialSerializer.h"
#include "ECS/Components.h"                // MaterialComponent
#include "System/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#ifdef _DEBUG
#pragma comment(lib, "assimp-vc143-mtd.lib")
#else
#pragma comment(lib, "assimp-vc143-mt.lib")
#endif

#include <DirectXMath.h>
#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace DirectX;

namespace Resource
{
    // -------------------------------------------------------------------------
    // Packed vertex — 48-byte layout (the new format, with tangent).
    //   pos    : float3   (offset  0, size 12)
    //   normal : float3   (offset 12, size 12)
    //   uv0    : float2   (offset 24, size  8)
    //   tangent: float4   (offset 32, size 16)  xyz = tangent direction,
    //                                            w   = bitangent handedness (±1)
    //
    // Older .meshlib files exported by previous SceneImporter versions are
    // 32 bytes (no tangent) — the loader still accepts them and falls back to
    // GBuffer.vs's synthesised TBN. Imports done with this version always emit
    // 48-byte vertices and set MeshLibraryMetadata.flags |= MESHLIB_FLAG_HAS_TANGENT.
    //
    // The pre-existing `BuildImsh` path (legacy single-mesh .imsh) keeps the
    // 32-byte format — it's not actively used by new imports and changing it
    // would break a separate file format. The active import path is
    // `BuildMeshLibBlob`, which produces .meshlib.
    //
    // CRITICAL — anonymous namespace gives the struct internal linkage. Without
    // it we'd collide with `Resource::PackedVertex` defined in PmxImporter.cpp
    // (32 bytes) and SceneInstanceLoader.cpp uses anonymous already. Linker
    // ICF / COMDAT folding then merges `vector<Resource::PackedVertex>`
    // instantiations across TUs onto whichever sizeof it found first — if
    // that's the 32-byte version, reserve(N) allocates N*32 bytes while
    // push_back writes 48-byte structs and the vector buffer overruns at
    // exactly capacity*32/48 elements (Sponza: 184402*32/48 ≈ 122934). The
    // resulting AV at index ~122962 is what surfaced this ODR violation.
    // -------------------------------------------------------------------------
    namespace {
        struct PackedVertex
        {
            float px, py, pz;     // position    (12 B)
            float nx, ny, nz;     // normal      (12 B)
            float u,  v;          // UV0         ( 8 B)
            float tx, ty, tz, tw; // tangent xyz + handedness  (16 B)
        };
        static_assert(sizeof(PackedVertex) == 48, "PackedVertex must be 48 bytes");

        // Legacy 32-byte vertex retained for the older single-mesh .imsh
        // exporter (BuildImsh below). Not used by the new meshlib path.
        struct PackedVertexLegacy
        {
            float px, py, pz;
            float nx, ny, nz;
            float u,  v;
        };
        static_assert(sizeof(PackedVertexLegacy) == 32, "PackedVertexLegacy must be 32 bytes");
    } // anonymous

    // -------------------------------------------------------------------------
    // Sanitise a node/mesh name for use in filenames and the .iscn text:
    //   - replace spaces and path separators with '_'
    //   - keep ASCII alphanumerics, '-', '.', '_'
    //   - pass through non-ASCII bytes (>= 0x80) unchanged — they are valid
    //     UTF-8 lead/continuation bytes for Japanese, Chinese, etc.
    // -------------------------------------------------------------------------
    static std::string SanitiseName(const std::string& raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (unsigned char c : raw)
        {
            if (c == ' ' || c == '/' || c == '\\' || c == ':')
                out += '_';
            else if (c > 127)
                out += static_cast<char>(c);  // UTF-8 multi-byte sequence byte
            else if (std::isalnum(c) || c == '-' || c == '.' || c == '_')
                out += static_cast<char>(c);
            // drop other ASCII control / special chars
        }
        return out.empty() ? "node" : out;
    }

    // -------------------------------------------------------------------------
    // Build a .imsh blob from a single aiMesh.
    // Produces: [AssetHeader][MeshMetadata][packed vertex data][uint32 index data]
    // flipX: negate X position/normal and flip triangle winding to convert
    //        right-handed (PMX/MMD) to DirectX left-handed coordinate space.
    //
    // The .imsh format stays at 32 bytes/vertex (PackedVertexLegacy). It's a
    // separate single-mesh container, only consumed by older code paths; new
    // scene imports flow through BuildMeshLibBlob which uses the 48-byte format.
    // -------------------------------------------------------------------------
    static std::vector<uint8_t> BuildImsh(const aiMesh* ai, bool flipX = false)
    {
        assert(ai);
        const uint32_t nv = ai->mNumVertices;
        const bool hasNor = ai->HasNormals();
        const bool hasUV  = ai->HasTextureCoords(0);

        // Pack interleaved vertices (legacy 32-byte format).
        std::vector<PackedVertexLegacy> verts(nv);
        for (uint32_t i = 0; i < nv; ++i)
        {
            auto& v = verts[i];
            v.px = ai->mVertices[i].x;
            v.py = ai->mVertices[i].y;
            v.pz = ai->mVertices[i].z;
            v.nx = hasNor ? ai->mNormals[i].x : 0.f;
            v.ny = hasNor ? ai->mNormals[i].y : 1.f;
            v.nz = hasNor ? ai->mNormals[i].z : 0.f;
            v.u  = hasUV  ? ai->mTextureCoords[0][i].x : 0.f;
            v.v  = hasUV  ? ai->mTextureCoords[0][i].y : 0.f;
            if (flipX) { v.px = -v.px; v.nx = -v.nx; }
        }

        // Collect uint32 indices (Assimp guarantees triangulated faces)
        std::vector<uint32_t> indices;
        indices.reserve(ai->mNumFaces * 3);
        for (uint32_t f = 0; f < ai->mNumFaces; ++f)
        {
            const aiFace& face = ai->mFaces[f];
            for (unsigned j = 0; j < face.mNumIndices; ++j)
                indices.push_back(face.mIndices[j]);
        }
        // Flip winding to preserve back-face culling after X-negation.
        if (flipX)
        {
            for (size_t f = 0; f + 2 < indices.size(); f += 3)
                std::swap(indices[f + 1], indices[f + 2]);
        }

        const uint32_t vbBytes = nv * sizeof(PackedVertexLegacy);
        const uint32_t ibBytes = static_cast<uint32_t>(indices.size()) * sizeof(uint32_t);

        MeshMetadata meta{};
        meta.vertexCount  = nv;
        meta.indexCount   = static_cast<uint32_t>(indices.size());
        meta.vertexStride = sizeof(PackedVertexLegacy);
        meta.indexStride  = 4;
        meta.subMeshCount = 1;
        meta.padding      = 0;

        AssetHeader header{};
        header.magic        = MAGIC_MESH;
        header.version      = ASSET_VERSION;
        header.resourceType = static_cast<uint16_t>(ResourceType::Mesh);
        header.metadataSize = sizeof(MeshMetadata);
        header.dataSize     = vbBytes + ibBytes;

        const size_t total = sizeof(AssetHeader) + sizeof(MeshMetadata) + vbBytes + ibBytes;
        std::vector<uint8_t> blob(total);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header, sizeof(AssetHeader));     dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(MeshMetadata));    dst += sizeof(MeshMetadata);
        std::memcpy(dst, verts.data(),   vbBytes);           dst += vbBytes;
        std::memcpy(dst, indices.data(), ibBytes);

        return blob;
    }


    // -------------------------------------------------------------------------
    // Build a .meshlib blob from N aiMesh inputs (P1-P6 rewrite replacement
    // for BuildImshMerged). Each aiMesh becomes one first-class MeshLibraryEntry
    // with its own vertex range, index range, AABB, and default material index.
    // Scene-graph nodes reference meshes by meshId (index into entries[]).
    //
    // On-disk layout (matches AssetHeader::MeshLibraryMetadata):
    //   [AssetHeader][MeshLibraryMetadata]
    //   [MeshLibraryEntry × N]
    //   [PackedVertex × totalVerts]
    //   [uint32 × totalIndices]
    //
    // `aiMaterialIdx[i]` becomes MeshLibraryEntry::defaultMaterialIdx directly —
    // it points at the scene's .imat registry and is resolved at scene-load time.
    // -------------------------------------------------------------------------
    static std::vector<uint8_t> BuildMeshLibBlob(
        const std::vector<const aiMesh*>& sources,
        const std::vector<uint32_t>&      aiMaterialIdx,
        bool                              flipX = false)
    {
        assert(sources.size() == aiMaterialIdx.size());
        if (sources.empty()) return {};

        uint32_t totalVerts = 0, totalIdx = 0;
        for (const aiMesh* ai : sources)
        {
            totalVerts += ai ? ai->mNumVertices   : 0u;
            totalIdx   += ai ? ai->mNumFaces * 3u : 0u;
        }
        if (totalVerts == 0 || totalIdx == 0) return {};

        // ---- Determine the library-wide vertex layout from the sources ------
        // The shared VB has ONE interleaved layout, so a stream is included for
        // the whole library if ANY source mesh carries it. Tangent is always
        // present (aiProcess_CalcTangentSpace fills it, else a (1,0,0,1)
        // sentinel). uv1 (2nd UV set) and per-vertex color are opt-in per the
        // source content, preserved instead of silently dropped.
        bool anyUV1 = false, anyColor = false;
        for (const aiMesh* ai : sources)
        {
            if (!ai) continue;
            if (ai->HasTextureCoords(1)) anyUV1   = true;
            if (ai->HasVertexColors(0))  anyColor = true;
        }

        uint32_t libFlags = MESHLIB_FLAG_HAS_TANGENT;
        if (anyUV1)   libFlags |= MESHLIB_FLAG_HAS_UV1;
        if (anyColor) libFlags |= MESHLIB_FLAG_HAS_COLOR;
        const MeshLibVertexLayout L = ComputeMeshLibVertexLayout(libFlags);

        std::vector<uint8_t>          vbytes(size_t(totalVerts) * L.stride, 0u);
        std::vector<uint32_t>         indices; indices.reserve(totalIdx);
        std::vector<MeshLibraryEntry> entries; entries.reserve(sources.size());

        // Interleaved byte-writer into the shared VB.
        auto wF = [](uint8_t* p, float f) { std::memcpy(p, &f, sizeof(float)); };

        uint32_t vOffset = 0;  // running base vertex for re-indexing into the
                               // library-wide pool
        uint32_t iOffset = 0;  // running index offset (= entries.back().indexStart)
        uint32_t maxUVSets = 0;  // diagnostic only
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const aiMesh* ai = sources[s];
            if (!ai || ai->mNumVertices == 0 || ai->mNumFaces == 0) continue;

            const bool hasNor   = ai->HasNormals();
            const bool hasUV    = ai->HasTextureCoords(0);
            const bool hasUV1   = ai->HasTextureCoords(1);
            const bool hasTan   = ai->HasTangentsAndBitangents();
            const bool hasColor = ai->HasVertexColors(0);
            if (ai->GetNumUVChannels() > maxUVSets) maxUVSets = ai->GetNumUVChannels();

            const uint32_t vStart = vOffset;
            const uint32_t iStart = iOffset;

            // Compute per-mesh AABB while packing vertices — one pass.
            float bmin[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
            float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

            for (uint32_t i = 0; i < ai->mNumVertices; ++i)
            {
                uint8_t* vp = vbytes.data() + size_t(vOffset + i) * L.stride;

                float px = ai->mVertices[i].x;
                float py = ai->mVertices[i].y;
                float pz = ai->mVertices[i].z;
                float nx = hasNor ? ai->mNormals[i].x : 0.f;
                float ny = hasNor ? ai->mNormals[i].y : 1.f;
                float nz = hasNor ? ai->mNormals[i].z : 0.f;
                float u  = hasUV  ? ai->mTextureCoords[0][i].x : 0.f;
                float vt = hasUV  ? ai->mTextureCoords[0][i].y : 0.f;

                // Tangent + handedness. Assimp gives us T and B as separate
                // vectors; the engine stores T.xyz and a single sign for B
                // recovery (B = cross(N, T) * sign), derived from whether the
                // authored bitangent agrees with cross(N, T) or its negation.
                // (1,0,0,1) sentinel when absent — matches FETCH_TANGENT4's
                // fallback so GBuffer.vs synthesises a basis from N.
                float tx = 1.f, ty = 0.f, tz = 0.f, tw = 1.f;
                if (hasTan && hasNor)
                {
                    const aiVector3D& T = ai->mTangents[i];
                    const aiVector3D& B = ai->mBitangents[i];
                    const float NxT_x = ny * T.z - nz * T.y;
                    const float NxT_y = nz * T.x - nx * T.z;
                    const float NxT_z = nx * T.y - ny * T.x;
                    const float sign  = (NxT_x * B.x + NxT_y * B.y + NxT_z * B.z) < 0.f ? -1.f : 1.f;
                    tx = T.x; ty = T.y; tz = T.z; tw = sign;
                }

                if (flipX)
                {
                    px = -px; nx = -nx;
                    // Mirroring X flips tangent X and inverts handedness.
                    tx = -tx; tw = -tw;
                }

                wF(vp + L.posOffset + 0, px);
                wF(vp + L.posOffset + 4, py);
                wF(vp + L.posOffset + 8, pz);
                wF(vp + L.normalOffset + 0, nx);
                wF(vp + L.normalOffset + 4, ny);
                wF(vp + L.normalOffset + 8, nz);
                wF(vp + L.uv0Offset + 0, u);
                wF(vp + L.uv0Offset + 4, vt);

                if (L.tangentOffset != 0xFFFFFFFFu)
                {
                    wF(vp + L.tangentOffset + 0,  tx);
                    wF(vp + L.tangentOffset + 4,  ty);
                    wF(vp + L.tangentOffset + 8,  tz);
                    wF(vp + L.tangentOffset + 12, tw);
                }
                if (L.uv1Offset != 0xFFFFFFFFu)
                {
                    // Default to uv0 when this particular mesh lacks a 2nd set,
                    // so a uv1-driven material still samples sane coordinates.
                    float u1 = hasUV1 ? ai->mTextureCoords[1][i].x : u;
                    float v1 = hasUV1 ? ai->mTextureCoords[1][i].y : vt;
                    wF(vp + L.uv1Offset + 0, u1);
                    wF(vp + L.uv1Offset + 4, v1);
                }
                if (L.colorOffset != 0xFFFFFFFFu)
                {
                    uint32_t packed = 0xFFFFFFFFu;  // opaque white default
                    if (hasColor)
                    {
                        const aiColor4D& c = ai->mColors[0][i];
                        packed = PackColorRGBA8(c.r, c.g, c.b, c.a);
                    }
                    std::memcpy(vp + L.colorOffset, &packed, sizeof(uint32_t));
                }

                if (px < bmin[0]) bmin[0] = px;
                if (py < bmin[1]) bmin[1] = py;
                if (pz < bmin[2]) bmin[2] = pz;
                if (px > bmax[0]) bmax[0] = px;
                if (py > bmax[1]) bmax[1] = py;
                if (pz > bmax[2]) bmax[2] = pz;
            }

            for (uint32_t f = 0; f < ai->mNumFaces; ++f)
            {
                const aiFace& face = ai->mFaces[f];
                for (unsigned j = 0; j < face.mNumIndices; ++j)
                    indices.push_back(face.mIndices[j] + vOffset);
            }
            // Winding flip for flipX mode — Assimp Triangulate+SortByPType
            // guarantees triangle-list layout, so strides of 3 are safe.
            if (flipX)
            {
                for (size_t f = iStart; f + 2 < indices.size(); f += 3)
                    std::swap(indices[f + 1], indices[f + 2]);
            }

            MeshLibraryEntry entry{};
            entry.vertexStart        = vStart;
            entry.vertexCount        = ai->mNumVertices;
            entry.indexStart         = iStart;
            entry.indexCount         = static_cast<uint32_t>(indices.size()) - iStart;
            entry.aabbMin[0]         = bmin[0]; entry.aabbMin[1] = bmin[1]; entry.aabbMin[2] = bmin[2];
            entry.aabbMax[0]         = bmax[0]; entry.aabbMax[1] = bmax[1]; entry.aabbMax[2] = bmax[2];
            entry.defaultMaterialIdx = aiMaterialIdx[s];
            entry.flags              = 0;
            entries.push_back(entry);

            vOffset += ai->mNumVertices;
            iOffset  = static_cast<uint32_t>(indices.size());
        }

        // Trim trailing capacity for any source that was skipped after the
        // up-front totalVerts tally (callers pre-filter, so vOffset normally
        // equals totalVerts — this keeps vertexCount, the VB byte length, and
        // MeshLibrary::Load's index-blob offset mutually consistent regardless).
        vbytes.resize(size_t(vOffset) * L.stride);

        if (vbytes.empty() || indices.empty() || entries.empty()) return {};

        LOG_INFO("SceneImporter: meshlib layout stride=%u flags=0x%X [tangent%s uv1%s color%s], "
                 "max UV sets in source=%u",
                 L.stride, libFlags,
                 (libFlags & MESHLIB_FLAG_HAS_TANGENT) ? "+" : "-",
                 (libFlags & MESHLIB_FLAG_HAS_UV1)     ? "+" : "-",
                 (libFlags & MESHLIB_FLAG_HAS_COLOR)   ? "+" : "-",
                 maxUVSets);
        if (maxUVSets > 2)
            LOG_WARNING("SceneImporter: source carries %u UV sets — only uv0 + uv1 are "
                        "preserved (extra sets dropped)", maxUVSets);

        const uint32_t entryBytes  = static_cast<uint32_t>(entries.size() * sizeof(MeshLibraryEntry));
        const uint32_t vbBytes     = static_cast<uint32_t>(vbytes.size());
        const uint32_t ibBytes     = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));

        MeshLibraryMetadata meta{};
        meta.meshCount    = static_cast<uint32_t>(entries.size());
        meta.vertexCount  = vOffset;
        meta.indexCount   = static_cast<uint32_t>(indices.size());
        meta.vertexStride = static_cast<uint16_t>(L.stride);
        meta.indexStride  = 4;
        meta.flags        = libFlags;
        meta.reserved     = 0;

        AssetHeader header{};
        header.magic        = MAGIC_MESHLIB;
        header.version      = ASSET_VERSION;
        header.resourceType = static_cast<uint16_t>(ResourceType::MeshLibrary);
        header.metadataSize = sizeof(MeshLibraryMetadata);
        header.dataSize     = entryBytes + vbBytes + ibBytes;

        const size_t total = sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)
                           + entryBytes + vbBytes + ibBytes;
        std::vector<uint8_t> blob(total);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header,        sizeof(AssetHeader));         dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,          sizeof(MeshLibraryMetadata)); dst += sizeof(MeshLibraryMetadata);
        std::memcpy(dst, entries.data(), entryBytes);                  dst += entryBytes;
        std::memcpy(dst, vbytes.data(),  vbBytes);                      dst += vbBytes;
        std::memcpy(dst, indices.data(), ibBytes);
        return blob;
    }

    // -------------------------------------------------------------------------
    // Node record used to build the .iscn payload.
    // -------------------------------------------------------------------------
    struct NodeRecord
    {
        int         index       = -1;
        int         parent      = -1;
        std::string name;           // sanitised
        float tx, ty, tz;           // translation
        float qx, qy, qz, qw;      // rotation quaternion
        float sx, sy, sz;           // scale
        std::vector<int> meshFileIndices;   // indices into meshFiles[]
    };

    // -------------------------------------------------------------------------
    // DFS: assign sequential indices to every node, decompose transforms,
    // collect mesh file references.
    // Returns the total count of node records appended to 'records'.
    // -------------------------------------------------------------------------
    static void CollectNodes(const aiNode*              node,
                             int                        parentIdx,
                             std::vector<NodeRecord>&   records)
    {
        const int myIdx = static_cast<int>(records.size());
        records.emplace_back();
        NodeRecord& rec = records.back();
        rec.index  = myIdx;
        rec.parent = parentIdx;
        rec.name   = SanitiseName(node->mName.C_Str()[0] ? node->mName.C_Str() : "node");

        // Decompose Assimp matrix → TRS
        // Assimp: v' = M * v  (column vectors, row-major storage)
        // We extract TRS in Assimp's own convention; SceneLoader will transpose.
        aiVector3D   aiTrans, aiScale;
        aiQuaternion aiRot;
        node->mTransformation.Decompose(aiScale, aiRot, aiTrans);

        rec.tx = aiTrans.x; rec.ty = aiTrans.y; rec.tz = aiTrans.z;
        rec.qx = aiRot.x;   rec.qy = aiRot.y;   rec.qz = aiRot.z; rec.qw = aiRot.w;
        rec.sx = aiScale.x; rec.sy = aiScale.y; rec.sz = aiScale.z;

        // Mesh reference indices are filled in later (after all .imsh files are named).
        // We store the raw aiMesh indices for now and remap below.
        for (unsigned m = 0; m < node->mNumMeshes; ++m)
            rec.meshFileIndices.push_back(static_cast<int>(node->mMeshes[m]));

        for (unsigned c = 0; c < node->mNumChildren; ++c)
            CollectNodes(node->mChildren[c], myIdx, records);
    }

    // -------------------------------------------------------------------------
    // Build the .iscn text payload.
    // meshFileNames[i] = the relative filename produced for scene mesh index i.
    // -------------------------------------------------------------------------
    // F-line record (one per "mesh reference"). Multiple F-lines may share
    // the same .imsh file when they belong to different submeshes of a
    // merged per-node mesh.
    struct MeshFileRef
    {
        std::string meshFile;   // relative filename of the .imsh
        std::string matFile;    // optional .imat
        uint32_t    subIdx;     // submesh index within meshFile (0 for legacy)
    };

    static std::string BuildIscnText(const std::string&              sourceStem,
                                     const std::vector<NodeRecord>&   nodes,
                                     const std::vector<MeshFileRef>&  fileRefs,
                                     const std::string&               skelFileName = "")
    {
        std::ostringstream ss;
        ss << "# ISCN engine scene container\n";
        ss << "S nodes=" << nodes.size()
           << " meshfiles=" << fileRefs.size()
           << " source=" << sourceStem << "\n";

        for (const NodeRecord& n : nodes)
        {
            ss << "N idx=" << n.index
               << " parent=" << n.parent
               << " tx=" << n.tx << " ty=" << n.ty << " tz=" << n.tz
               << " qx=" << n.qx << " qy=" << n.qy
               << " qz=" << n.qz << " qw=" << n.qw
               << " sx=" << n.sx << " sy=" << n.sy << " sz=" << n.sz
               << " mc=" << n.meshFileIndices.size();

            for (int k = 0; k < static_cast<int>(n.meshFileIndices.size()); ++k)
                ss << " mr" << k << "=" << n.meshFileIndices[k];

            ss << " name=" << n.name << "\n";
        }

        for (int i = 0; i < static_cast<int>(fileRefs.size()); ++i)
        {
            // New format: `mesh=K` is the MeshLibraryEntry index inside the
            // .meshlib referenced by `file=`. Replaces the legacy `sub=K`
            // submesh-in-merged-.imsh scheme.
            ss << "F idx=" << i << " file=" << fileRefs[i].meshFile
               << " mesh=" << fileRefs[i].subIdx;
            if (!fileRefs[i].matFile.empty())
                ss << " mat=" << fileRefs[i].matFile;
            ss << "\n";
        }

        if (!skelFileName.empty())
            ss << "K file=" << skelFileName << "\n";

        return ss.str();
    }

    // -------------------------------------------------------------------------
    // Build the .iscn AssetHeader + SceneMetadata + text blob.
    // -------------------------------------------------------------------------
    static std::vector<uint8_t> BuildIscnBlob(const std::string&              sourceStem,
                                               const std::vector<NodeRecord>&  nodes,
                                               const std::vector<MeshFileRef>& fileRefs,
                                               const std::string&              skelFileName = "")
    {
        const std::string text  = BuildIscnText(sourceStem, nodes, fileRefs, skelFileName);
        const uint32_t textLen  = static_cast<uint32_t>(text.size());
        const uint32_t payloadSz = textLen + 1;  // +1 for null terminator

        SceneMetadata meta{};
        meta.nodeCount     = static_cast<uint32_t>(nodes.size());
        meta.meshFileCount = static_cast<uint32_t>(fileRefs.size());
        meta.textLength    = textLen;
        meta.reserved      = 0;

        AssetHeader header{};
        header.magic        = MAGIC_SCENE;
        header.version      = ASSET_VERSION;
        header.resourceType = static_cast<uint16_t>(ResourceType::Scene);
        header.metadataSize = sizeof(SceneMetadata);
        header.dataSize     = payloadSz;

        const size_t total = sizeof(AssetHeader) + sizeof(SceneMetadata) + payloadSz;
        std::vector<uint8_t> blob(total, 0);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header, sizeof(AssetHeader));    dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(SceneMetadata));  dst += sizeof(SceneMetadata);
        std::memcpy(dst, text.c_str(), textLen);
        // last byte already 0 (null terminator)

        return blob;
    }

    // -------------------------------------------------------------------------
    // Serialise a SkeletonImportResult into a .iskel binary blob.
    //
    // Payload layout:
    //   uint32   meshBlendCount
    //   int32    parentIndex[boneCount]
    //   float    inverseBindPose[boneCount * 16]
    //   float    bindPose[boneCount * 16]
    //   float    restPoseLocal[boneCount * 16]
    //   char     boneNames[boneCount * 64]
    //   -- clipCount clips: --
    //     uint32 boneCount, frameCount; float duration, frameRate
    //     float3 positions[boneCount*frameCount]
    //     float4 rotations[boneCount*frameCount]
    //     float3 scales[boneCount*frameCount]
    //   -- meshBlendCount entries: --
    //     uint32 meshIndex, vertexCount; BlendVertex[vertexCount]
    // -------------------------------------------------------------------------
    // FlipSkeletonX — converts PMX right-handed skeleton data to DX left-handed
    // by reflecting across the YZ plane (negate X).
    //   4x4 matrices : T' = M * T * M   where M = Scale(-1,1,1)
    //   positions    : x = -x
    //   rotations    : Q' = (qx, -qy, -qz, qw)  (conjugate XY spin axes)
    // -------------------------------------------------------------------------
    static void FlipSkeletonX(SkeletonImportResult& skel)
    {
        using namespace DirectX;
        const XMMATRIX M = XMMatrixScaling(-1.f, 1.f, 1.f);

        auto FlipMat = [&](XMFLOAT4X4& mat)
        {
            XMMATRIX T = XMLoadFloat4x4(&mat);
            XMStoreFloat4x4(&mat, M * T * M);
        };

        for (uint32_t b = 0; b < skel.skeleton.boneCount; ++b)
        {
            FlipMat(skel.skeleton.inverseBindPose[b]);
            FlipMat(skel.skeleton.bindPose[b]);
            FlipMat(skel.skeleton.restPoseLocal[b]);
        }

        for (ClipAsset& clip : skel.clips)
        {
            const uint32_t total = clip.boneCount * clip.frameCount;
            for (uint32_t i = 0; i < total; ++i)
            {
                clip.positions[i].x = -clip.positions[i].x;
                // Q' = (qx, -qy, -qz, qw)
                clip.rotations[i].y = -clip.rotations[i].y;
                clip.rotations[i].z = -clip.rotations[i].z;
            }
        }
    }

    // -------------------------------------------------------------------------
    static std::vector<uint8_t> BuildIskelBlob(const SkeletonImportResult& skel,
                                                uint32_t numSceneMeshes)
    {
        const SkeletonAsset& sk   = skel.skeleton;
        const uint32_t boneCount  = sk.boneCount;
        const uint32_t clipCount  = static_cast<uint32_t>(skel.clips.size());

        // Count meshes that have non-empty blend data
        uint32_t meshBlendCount = 0;
        for (uint32_t mi = 0; mi < numSceneMeshes; ++mi)
        {
            if (mi < static_cast<uint32_t>(skel.perMeshBlendData.size()) &&
                !skel.perMeshBlendData[mi].empty())
                ++meshBlendCount;
        }

        // --- Compute payload byte size ---
        size_t payloadSz = sizeof(uint32_t);  // meshBlendCount
        payloadSz += boneCount * sizeof(int32_t);          // parentIndex
        payloadSz += boneCount * 16 * sizeof(float);       // inverseBindPose
        payloadSz += boneCount * 16 * sizeof(float);       // bindPose
        payloadSz += boneCount * 16 * sizeof(float);       // restPoseLocal
        payloadSz += boneCount * 64;                        // boneNames
        payloadSz += boneCount * 6 * sizeof(float);        // boneRestAABBs (min3 + max3)

        for (const ClipAsset& c : skel.clips)
        {
            const size_t kf = (size_t)c.boneCount * c.frameCount;
            payloadSz += 2 * sizeof(uint32_t) + 2 * sizeof(float); // header
            payloadSz += kf * 3 * sizeof(float);  // positions
            payloadSz += kf * 4 * sizeof(float);  // rotations
            payloadSz += kf * 3 * sizeof(float);  // scales
        }

        for (uint32_t mi = 0; mi < numSceneMeshes; ++mi)
        {
            if (mi >= static_cast<uint32_t>(skel.perMeshBlendData.size())) break;
            const auto& bd = skel.perMeshBlendData[mi];
            if (!bd.empty())
                payloadSz += 2 * sizeof(uint32_t) + bd.size() * sizeof(BlendVertex);
        }

        // --- Build header + metadata ---
        SkeletonMetadata meta{};
        meta.boneCount   = boneCount;
        meta.clipCount   = clipCount;
        meta.reserved[0] = meshBlendCount;  // re-purpose first reserved slot
        meta.reserved[1] = 0;

        AssetHeader header{};
        header.magic        = MAGIC_SKELETON;
        header.version      = ASSET_VERSION;
        header.resourceType = static_cast<uint16_t>(ResourceType::Skeleton);
        header.metadataSize = sizeof(SkeletonMetadata);
        header.dataSize     = static_cast<uint32_t>(payloadSz);
        header.flags        = 0;
        header.reserved     = 0;

        const size_t totalSz = sizeof(AssetHeader) + sizeof(SkeletonMetadata) + payloadSz;
        std::vector<uint8_t> blob(totalSz, 0);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header, sizeof(AssetHeader));      dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(SkeletonMetadata)); dst += sizeof(SkeletonMetadata);

        // meshBlendCount
        std::memcpy(dst, &meshBlendCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

        // parentIndex
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.parentIndex[i], sizeof(int32_t)); dst += sizeof(int32_t); }

        // inverseBindPose
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.inverseBindPose[i], 16 * sizeof(float)); dst += 16 * sizeof(float); }

        // bindPose
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.bindPose[i], 16 * sizeof(float)); dst += 16 * sizeof(float); }

        // restPoseLocal
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.restPoseLocal[i], 16 * sizeof(float)); dst += 16 * sizeof(float); }

        // boneNames
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, sk.boneNames[i], 64); dst += 64; }

        // boneRestAABBs (6 floats per bone: min xyz + max xyz)
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            std::memcpy(dst, &sk.boneRestAABBs[i].localMin, 3 * sizeof(float)); dst += 3 * sizeof(float);
            std::memcpy(dst, &sk.boneRestAABBs[i].localMax, 3 * sizeof(float)); dst += 3 * sizeof(float);
        }

        // clips
        for (const ClipAsset& c : skel.clips)
        {
            const uint32_t bc = c.boneCount, fc = c.frameCount;
            const size_t   kf = (size_t)bc * fc;
            std::memcpy(dst, &bc,          sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &fc,          sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &c.duration,  sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, &c.frameRate, sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, c.positions.data(), kf * 3 * sizeof(float)); dst += kf * 3 * sizeof(float);
            std::memcpy(dst, c.rotations.data(), kf * 4 * sizeof(float)); dst += kf * 4 * sizeof(float);
            std::memcpy(dst, c.scales.data(),    kf * 3 * sizeof(float)); dst += kf * 3 * sizeof(float);
        }

        // per-mesh blend data
        for (uint32_t mi = 0; mi < numSceneMeshes; ++mi)
        {
            if (mi >= static_cast<uint32_t>(skel.perMeshBlendData.size())) break;
            const auto& bd = skel.perMeshBlendData[mi];
            if (bd.empty()) continue;
            const uint32_t vc = static_cast<uint32_t>(bd.size());
            std::memcpy(dst, &mi, sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &vc, sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, bd.data(), vc * sizeof(BlendVertex)); dst += vc * sizeof(BlendVertex);
        }

        return blob;
    }

    // =========================================================================
    // SceneImporter::Import
    // =========================================================================
    SceneImporter::ImportResult SceneImporter::Import(const std::string&          sourcePath,
                                                       const std::vector<uint8_t>& sourceData)
    {
        ImportResult result;

        if (sourceData.empty())
        {
            LOG_ERROR("SceneImporter: empty source data for '%s'", sourcePath.c_str());
            return result;
        }

        // ---- Load with Assimp from memory ----
        // Pass the file extension as format hint.
        std::string ext = std::filesystem::path(sourcePath).extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // VRM is a GLB container with custom extension JSON — Assimp parses it as glb.
        if (ext == ".vrm") ext = ".glb";
        const char* hint = ext.empty() ? "" : ext.c_str() + 1;  // strip leading '.'

        Assimp::Importer importer;
        constexpr unsigned kFlags =
            aiProcess_Triangulate            |
            aiProcess_ConvertToLeftHanded    |
            aiProcess_GenSmoothNormals       |
            aiProcess_CalcTangentSpace       |  // ensures mTangents / mBitangents populated
            aiProcess_JoinIdenticalVertices  |
            aiProcess_SortByPType;

        const aiScene* scene = importer.ReadFileFromMemory(
            sourceData.data(), sourceData.size(), kFlags, hint);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        {
            LOG_ERROR("SceneImporter: Assimp failed on '%s': %s",
                      sourcePath.c_str(), importer.GetErrorString());
            return result;
        }

        const std::string stem = std::filesystem::path(sourcePath).stem().string();

        //LOG_INFO("SceneImporter: processing '%s' — %u mesh(es)%s",
        //         sourcePath.c_str(), scene->mNumMeshes, isPmx ? " [PMX flipX]" : "");

        // ---- Skeleton extraction (optional) ----
        SkeletonImportResult skelResult;
        const bool hasSkel = SkeletonImporter::Import(scene, skelResult);
        std::string skelFileName;  // empty = no skeleton

        if (hasSkel)
        {
            LOG_INFO("SceneImporter: skeleton detected — %u bones, %zu clips",
                     skelResult.skeleton.boneCount, skelResult.clips.size());

            skelFileName = stem + ".iskel";  // bare filename, sibling of .iscn

            std::vector<uint8_t> iskelBlob =
                BuildIskelBlob(skelResult, scene->mNumMeshes);

            const std::string iskelRel = stem + "/" + skelFileName;
            result.files.push_back({ iskelRel, std::move(iskelBlob) });

            LOG_INFO("SceneImporter:   skeleton → '%s'", iskelRel.c_str());
        }

        // ---- Traverse scene graph FIRST so we know per-node aiMesh groupings.
        // Previous version built one .imsh per aiMesh regardless of hierarchy,
        // which (for Bistro-class scenes) produced 22k tiny files because
        // Assimp splits every node's mesh by material. Now each SCENE NODE
        // gets one merged .imsh — matching the user's "one source mesh ⇒ one
        // engine file" expectation.
        std::vector<NodeRecord> nodes;
        nodes.reserve(32);
        CollectNodes(scene->mRootNode, -1, nodes);

        // ---- Generate .imat for each Assimp material (material files are
        //      referenced per-F-line, and aiMaterial indices stay the same
        //      regardless of per-node merging). ----
        std::vector<std::string> matFileNameByAiMat(scene->mNumMaterials);
        {
            std::unordered_map<unsigned, std::string> matIdxToFile;
            for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
            {
                const aiMesh* ai = scene->mMeshes[mi];
                const unsigned matIdx = ai->mMaterialIndex;
                if (matIdxToFile.count(matIdx)) continue;

                const aiMaterial* aiMat = scene->mMaterials[matIdx];
                MaterialComponent mc{};
                aiString aiName;
                aiMat->Get(AI_MATKEY_NAME, aiName);
                std::string matName = SanitiseName(
                    aiName.length > 0 ? aiName.C_Str()
                                      : ("material_" + std::to_string(matIdx)).c_str());

                const std::string matFileName = "Materials/" + matName + ".imat";
                const std::string matRelPath  = stem + "/" + matFileName;
                result.files.push_back({ matRelPath, Resource::BuildMaterialBlob(mc) });

                matIdxToFile[matIdx]          = matFileName;
                matFileNameByAiMat[matIdx]    = matFileName;
            }
            LOG_INFO("SceneImporter: generated %zu .imat files", matIdxToFile.size());
        }

        // ---- Merge EVERY aiMesh into ONE .imsh for the whole scene -------------
        // One file per scene — node management lives entirely in the .iscn.
        // Each F-line points to a submesh index inside the single merged
        // .imsh. Simplest layout that preserves original FBX object identity:
        // submesh K in the big .imsh IS aiMesh K (modulo empty-mesh skips).
        std::vector<const aiMesh*> allMeshes;
        std::vector<uint32_t>      allMatIdx;
        std::vector<int>           aiMeshToSubIdx(scene->mNumMeshes, -1);
        allMeshes.reserve(scene->mNumMeshes);
        allMatIdx.reserve(scene->mNumMeshes);

        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* ai = scene->mMeshes[mi];
            if (!ai || ai->mNumVertices == 0 || ai->mNumFaces == 0)
            {
                LOG_WARNING("SceneImporter: aiMesh[%u] empty — skipping", mi);
                continue;
            }
            aiMeshToSubIdx[mi] = static_cast<int>(allMeshes.size());
            allMeshes.push_back(ai);
            allMatIdx.push_back(ai->mMaterialIndex);
        }

        std::vector<MeshFileRef> fileRefs;
        const std::string mergedBase = stem + ".meshlib";
        if (!allMeshes.empty())
        {
            // New path (P1-P6 rewrite): single .meshlib with per-aiMesh
            // first-class entries. flipX=false because
            // aiProcess_ConvertToLeftHanded already handled coord-space flip.
            std::vector<uint8_t> blob = BuildMeshLibBlob(allMeshes, allMatIdx, false);
            if (blob.empty())
            {
                LOG_ERROR("SceneImporter: BuildMeshLibBlob failed on '%s'",
                          sourcePath.c_str());
                return result;
            }
            result.files.push_back({ stem + "/" + mergedBase, std::move(blob) });

            // One F-line per non-empty aiMesh. All F-lines share the same
            // `file=` (the one .meshlib); `mesh=K` picks the MeshLibraryEntry
            // by its stable meshId.
            fileRefs.reserve(allMeshes.size());
            for (size_t s = 0; s < allMeshes.size(); ++s)
            {
                MeshFileRef fr;
                fr.meshFile = mergedBase;
                fr.subIdx   = static_cast<uint32_t>(s);  // reused as meshId
                fr.matFile  = (allMatIdx[s] < matFileNameByAiMat.size())
                              ? matFileNameByAiMat[allMatIdx[s]] : "";
                fileRefs.push_back(std::move(fr));
            }
        }

        // Remap each node's meshFileIndices from aiMesh-idx → submesh-idx
        // (= F-line idx, since F-lines are emitted in submesh order). Drop
        // invalid refs (aiMesh was empty and got skipped).
        for (NodeRecord& nrec : nodes)
        {
            for (int& ref : nrec.meshFileIndices)
            {
                ref = (ref >= 0 && ref < static_cast<int>(aiMeshToSubIdx.size()))
                      ? aiMeshToSubIdx[ref] : -1;
            }
            nrec.meshFileIndices.erase(
                std::remove(nrec.meshFileIndices.begin(),
                            nrec.meshFileIndices.end(), -1),
                nrec.meshFileIndices.end());
        }

        // ---- Build and append .iscn blob (also in the subfolder) ----
        const std::string iscnRel  = stem + "/" + stem + ".iscn";
        std::vector<uint8_t> iscnBlob = BuildIscnBlob(stem, nodes, fileRefs, skelFileName);
        result.files.push_back({ iscnRel, std::move(iscnBlob) });

        result.success = true;
        LOG_INFO("SceneImporter: done — 1 .meshlib (%zu meshes) + 1 .iscn%s, %zu nodes",
                 fileRefs.size(),
                 hasSkel ? " + 1 .iskel" : "", nodes.size());
        return result;
    }

} // namespace Resource
