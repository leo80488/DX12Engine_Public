#include "Resource/MeshImporter.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"

#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource
{
    // -------------------------------------------------------------------------
    // Vertex layout: 32 bytes
    // -------------------------------------------------------------------------
    struct ObjVertex
    {
        float px, py, pz;   // position
        float nx, ny, nz;   // normal
        float u,  v;        // UV
    };
    static_assert(sizeof(ObjVertex) == 32, "ObjVertex must be 32 bytes");

    // -------------------------------------------------------------------------
    // Face-vertex key for vertex welding
    // -------------------------------------------------------------------------
    struct FaceVert
    {
        int pos, nor, uv;   // 1-based OBJ indices (0 means absent)
        bool operator==(const FaceVert& o) const
        {
            return pos == o.pos && nor == o.nor && uv == o.uv;
        }
    };

    struct FaceVertHash
    {
        size_t operator()(const FaceVert& f) const noexcept
        {
            // FNV-1a style combine
            size_t h = 2166136261u;
            auto mix = [&](int v) {
                h ^= static_cast<size_t>(v);
                h *= 16777619u;
            };
            mix(f.pos); mix(f.nor); mix(f.uv);
            return h;
        }
    };

    // -------------------------------------------------------------------------
    // Parse a face token "p", "p/t", "p//n", "p/t/n"  (all 1-based)
    // -------------------------------------------------------------------------
    static FaceVert ParseFaceToken(const char* tok)
    {
        FaceVert fv{ 0, 0, 0 };
        int pos = 0, uv = 0, nor = 0;
        // sscanf handles missing fields gracefully when format has / separators
        int count = sscanf_s(tok, "%d/%d/%d", &pos, &uv, &nor);
        if (count < 3)
        {
            // try "p//n"
            count = sscanf_s(tok, "%d//%d", &pos, &nor);
            uv = 0;
        }
        fv.pos = pos; fv.uv = uv; fv.nor = nor;
        return fv;
    }

    // -------------------------------------------------------------------------
    // MeshImporter::Import — OBJ → .imsh
    // -------------------------------------------------------------------------
    std::vector<uint8_t> MeshImporter::Import(const std::string& sourcePath,
                                               const std::vector<uint8_t>& sourceData)
    {
        if (sourceData.empty())
        {
            LOG_ERROR("MeshImporter: empty source data for '%s'", sourcePath.c_str());
            return {};
        }

        // ---- Parse OBJ text ----
        std::vector<std::array<float,3>> rawPos;
        std::vector<std::array<float,3>> rawNor;
        std::vector<std::array<float,2>> rawUV;

        // Each OBJ face produces a list of FaceVert (already triangulated below)
        struct Triangle { FaceVert v[3]; };
        std::vector<Triangle> triangles;

        std::string text(reinterpret_cast<const char*>(sourceData.data()), sourceData.size());
        std::istringstream ss(text);
        std::string line;

        while (std::getline(ss, line))
        {
            if (line.empty() || line[0] == '#') continue;

            // Remove \r if present
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            std::istringstream ls(line);
            std::string token;
            ls >> token;

            if (token == "v")
            {
                std::array<float,3> p{};
                ls >> p[0] >> p[1] >> p[2];
                rawPos.push_back(p);
            }
            else if (token == "vn")
            {
                std::array<float,3> n{};
                ls >> n[0] >> n[1] >> n[2];
                rawNor.push_back(n);
            }
            else if (token == "vt")
            {
                std::array<float,2> uv{};
                ls >> uv[0] >> uv[1];
                rawUV.push_back(uv);
            }
            else if (token == "f")
            {
                // Fan triangulate: v0, v1, v2,  v0, v2, v3, ...
                std::vector<FaceVert> fverts;
                std::string ftok;
                while (ls >> ftok)
                    fverts.push_back(ParseFaceToken(ftok.c_str()));

                for (size_t fi = 1; fi + 1 < fverts.size(); ++fi)
                {
                    Triangle tri;
                    tri.v[0] = fverts[0];
                    tri.v[1] = fverts[fi];
                    tri.v[2] = fverts[fi + 1];
                    triangles.push_back(tri);
                }
            }
        }

        if (rawPos.empty() || triangles.empty())
        {
            LOG_ERROR("MeshImporter: no geometry in '%s'", sourcePath.c_str());
            return {};
        }

        // ---- Weld vertices and build index buffer ----
        std::vector<ObjVertex>  vertices;
        std::vector<uint32_t>   indices;

        std::unordered_map<FaceVert, uint32_t, FaceVertHash> cache;

        auto resolvePos = [&](int idx) -> std::array<float,3> {
            if (idx <= 0 || idx > static_cast<int>(rawPos.size()))
                return {};
            return rawPos[idx - 1];
        };
        auto resolveNor = [&](int idx) -> std::array<float,3> {
            if (idx <= 0 || idx > static_cast<int>(rawNor.size()))
                return {};
            return rawNor[idx - 1];
        };
        auto resolveUV = [&](int idx) -> std::array<float,2> {
            if (idx <= 0 || idx > static_cast<int>(rawUV.size()))
                return {};
            auto uv = rawUV[idx - 1];
            uv[1] = 1.0f - uv[1]; // flip V for DX convention
            return uv;
        };

        for (const Triangle& tri : triangles)
        {
            for (int vi = 0; vi < 3; ++vi)
            {
                const FaceVert& fv = tri.v[vi];
                auto it = cache.find(fv);
                if (it != cache.end())
                {
                    indices.push_back(it->second);
                }
                else
                {
                    auto p  = resolvePos(fv.pos);
                    auto n  = resolveNor(fv.nor);
                    auto uv = resolveUV(fv.uv);

                    ObjVertex ov{};
                    ov.px = p[0]; ov.py = p[1]; ov.pz = p[2];
                    ov.nx = n[0]; ov.ny = n[1]; ov.nz = n[2];
                    ov.u  = uv[0]; ov.v = uv[1];

                    uint32_t idx = static_cast<uint32_t>(vertices.size());
                    vertices.push_back(ov);
                    cache[fv] = idx;
                    indices.push_back(idx);
                }
            }
        }

        // ---- Flat normal generation if OBJ had no normals ----
        const bool hasNormals = !rawNor.empty();
        if (!hasNormals)
        {
            // Zero all normals first
            for (auto& ov : vertices)
                ov.nx = ov.ny = ov.nz = 0.0f;

            for (size_t i = 0; i < indices.size(); i += 3)
            {
                uint32_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                ObjVertex& v0 = vertices[i0];
                ObjVertex& v1 = vertices[i1];
                ObjVertex& v2 = vertices[i2];

                float ax = v1.px - v0.px, ay = v1.py - v0.py, az = v1.pz - v0.pz;
                float bx = v2.px - v0.px, by = v2.py - v0.py, bz = v2.pz - v0.pz;
                float cx = ay*bz - az*by;
                float cy = az*bx - ax*bz;
                float cz = ax*by - ay*bx;
                float len = std::sqrt(cx*cx + cy*cy + cz*cz);
                if (len > 1e-6f) { cx /= len; cy /= len; cz /= len; }

                for (uint32_t idx : { i0, i1, i2 })
                {
                    vertices[idx].nx += cx;
                    vertices[idx].ny += cy;
                    vertices[idx].nz += cz;
                }
            }

            // Normalize accumulated normals
            for (auto& ov : vertices)
            {
                float len = std::sqrt(ov.nx*ov.nx + ov.ny*ov.ny + ov.nz*ov.nz);
                if (len > 1e-6f) { ov.nx /= len; ov.ny /= len; ov.nz /= len; }
            }
        }

        // ---- Build .imsh blob ----
        MeshMetadata meshMeta{};
        meshMeta.vertexCount  = static_cast<uint32_t>(vertices.size());
        meshMeta.indexCount   = static_cast<uint32_t>(indices.size());
        meshMeta.vertexStride = sizeof(ObjVertex);
        meshMeta.indexStride  = 4;  // uint32_t
        meshMeta.subMeshCount = 1;
        meshMeta.padding      = 0;

        const uint32_t vbBytes = meshMeta.vertexCount * meshMeta.vertexStride;
        const uint32_t ibBytes = meshMeta.indexCount  * meshMeta.indexStride;

        AssetHeader header{};
        header.magic        = MAGIC_MESH;
        header.version      = ASSET_VERSION;
        header.resourceType = static_cast<uint16_t>(ResourceType::Mesh);
        header.metadataSize = sizeof(MeshMetadata);
        header.dataSize     = vbBytes + ibBytes;
        header.flags        = 0;
        header.reserved     = 0;

        const size_t totalSize = sizeof(AssetHeader) + sizeof(MeshMetadata) + vbBytes + ibBytes;
        std::vector<uint8_t> blob(totalSize);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header,   sizeof(AssetHeader));   dst += sizeof(AssetHeader);
        std::memcpy(dst, &meshMeta, sizeof(MeshMetadata));  dst += sizeof(MeshMetadata);
        std::memcpy(dst, vertices.data(), vbBytes);          dst += vbBytes;
        std::memcpy(dst, indices.data(),  ibBytes);

        LOG_INFO("MeshImporter: imported '%s' → .imsh (%u verts, %u tris, %zu bytes)",
                 sourcePath.c_str(),
                 meshMeta.vertexCount,
                 meshMeta.indexCount / 3,
                 totalSize);

        return blob;
    }
}
