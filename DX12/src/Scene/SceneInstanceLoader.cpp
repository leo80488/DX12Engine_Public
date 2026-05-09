#include "Scene/SceneInstanceLoader.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "Resource/AssetManager.h"
#include "Resource/AssetFS.h"
#include "Resource/MeshLibrary.h"
#include "Resource/AssetHeader.h"
#include "Resource/ImshPack.h"
#include "Resource/SkeletonAsset.h"
#include "Resource/SkeletonImporter.h"
#include "Resource/MaterialSerializer.h"
#include "Graphics/Renderer.h"
#include "Scene/SceneLoader.h"   // SceneLoader::SkinnedMeshPending
#include "System/Log.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cfloat>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

using namespace DirectX;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------
namespace
{
    // Legacy interleaved vertex (32 bytes). Used to view the first three
    // attribute streams (pos / normal / uv) regardless of whether the asset
    // is the 32-byte legacy layout or the 48-byte with-tangent layout — the
    // first 32 bytes are byte-for-byte identical between the two, so reading
    // through this view works for both. Pointer arithmetic must use the
    // metadata's vertexStride, NOT sizeof(PackedVertex), to skip past the
    // optional trailing tangent in 48-byte assets.
    struct PackedVertex
    {
        float px, py, pz;   // position  (12 B)
        float nx, ny, nz;   // normal    (12 B)
        float u,  v;        // UV0       ( 8 B)
    };
    static_assert(sizeof(PackedVertex) == 32, "PackedVertex size mismatch");

    struct NodeRecord
    {
        int              idx    { -1 };
        int              parent { -1 };
        float tx { 0 }, ty { 0 }, tz { 0 };
        float qx { 0 }, qy { 0 }, qz { 0 }, qw { 1 };
        float sx { 1 }, sy { 1 }, sz { 1 };
        std::vector<int> meshFileIndices;  // indices into fileRecords[]
        std::string      name;
    };

    struct FileRecord
    {
        int         idx  { -1 };
        std::string file;          // bare filename (sibling of .iscn)
        std::string matFile;       // optional .imat filename (from mat= field)
        uint32_t    subIdx { 0 };  // submesh index within `file` (0 = legacy / single submesh)
    };

    // -----------------------------------------------------------------------
    // Parse one "N ..." line into a NodeRecord.
    // Format: N idx=i parent=j tx=.. ty=.. tz=.. qx=.. qy=.. qz=.. qw=..
    //                           sx=.. sy=.. sz=.. mc=k mr0=fi ... name=name
    // -----------------------------------------------------------------------
    bool ParseNLine(const std::string& line, NodeRecord& out)
    {
        std::istringstream ss(line.substr(2));  // skip "N "
        std::string token;
        while (ss >> token)
        {
            const auto eq = token.find('=');
            if (eq == std::string::npos) continue;

            const std::string key = token.substr(0, eq);
            const std::string val = token.substr(eq + 1);
            if (val.empty()) continue;

            try
            {
                if      (key == "idx")    out.idx    = std::stoi(val);
                else if (key == "parent") out.parent = std::stoi(val);
                else if (key == "tx")     out.tx     = std::stof(val);
                else if (key == "ty")     out.ty     = std::stof(val);
                else if (key == "tz")     out.tz     = std::stof(val);
                else if (key == "qx")     out.qx     = std::stof(val);
                else if (key == "qy")     out.qy     = std::stof(val);
                else if (key == "qz")     out.qz     = std::stof(val);
                else if (key == "qw")     out.qw     = std::stof(val);
                else if (key == "sx")     out.sx     = std::stof(val);
                else if (key == "sy")     out.sy     = std::stof(val);
                else if (key == "sz")     out.sz     = std::stof(val);
                else if (key == "mc")     { /* count, mesh refs follow */ }
                else if (key == "name")   out.name   = val;
                else if (key.size() > 2 && key[0] == 'm' && key[1] == 'r')
                    out.meshFileIndices.push_back(std::stoi(val));
            }
            catch (...) {}
        }
        return out.idx >= 0;
    }

    // -----------------------------------------------------------------------
    // Parse one "K ..." line — skeleton file reference.
    // Format: K file=filename.iskel
    // -----------------------------------------------------------------------
    bool ParseKLine(const std::string& line, std::string& outFile)
    {
        const size_t filePos = line.find("file=");
        if (filePos == std::string::npos) return false;
        outFile = line.substr(filePos + 5);
        // trim trailing whitespace / CR
        while (!outFile.empty() && (outFile.back() == '\r' || outFile.back() == '\n' ||
                                     outFile.back() == ' '))
            outFile.pop_back();
        return !outFile.empty();
    }

    // -----------------------------------------------------------------------
    // Parse one "F ..." line into a FileRecord.
    // Format: F idx=i file=filename.imsh
    // -----------------------------------------------------------------------
    bool ParseFLine(const std::string& line, FileRecord& out)
    {
        size_t idxPos = line.find("idx=");
        if(idxPos != std::string::npos) {
            size_t spaceAfterIdx = line.find(' ', idxPos);
            std::string val = line.substr(idxPos + 4, spaceAfterIdx - (idxPos + 4));
            try { out.idx = std::stoi(val); }
            catch (...) {}
        }
        // F line layout (new P1-P6 exporter):
        //   F idx=I file=<name.meshlib> mesh=M [mat=<name.imat>]
        // `mesh=M` is the MeshLibraryEntry index (meshId). Still accept the
        // legacy `sub=` keyword for transition scenes, stored in the same
        // subIdx field (semantically: index into the shared pool).
        size_t filePos = line.find("file=");
        size_t subPos  = line.find(" mesh=");
        if (subPos == std::string::npos) subPos = line.find(" sub=");
        size_t matPos  = line.find(" mat=");

        if (filePos != std::string::npos)
        {
            size_t fileStart = filePos + 5;
            size_t fileEnd   = std::string::npos;
            if (subPos != std::string::npos && subPos > fileStart) fileEnd = subPos;
            else if (matPos != std::string::npos && matPos > fileStart) fileEnd = matPos;

            out.file = (fileEnd == std::string::npos)
                ? line.substr(fileStart)
                : line.substr(fileStart, fileEnd - fileStart);

            while (!out.file.empty() && (out.file.back() == ' ' || out.file.back() == '\r' || out.file.back() == '\n'))
                out.file.pop_back();
        }

        if (subPos != std::string::npos)
        {
            // " mesh=" is 6 chars, " sub=" is 5 — detect which one we matched.
            const bool isMesh  = (line.compare(subPos, 6, " mesh=") == 0);
            const size_t skip  = isMesh ? 6u : 5u;
            size_t subStart    = subPos + skip;
            size_t subEnd      = (matPos != std::string::npos && matPos > subStart)
                                 ? matPos : line.find(' ', subStart);
            std::string val = (subEnd == std::string::npos)
                ? line.substr(subStart)
                : line.substr(subStart, subEnd - subStart);
            try { out.subIdx = static_cast<uint32_t>(std::stoul(val)); }
            catch (...) { out.subIdx = 0; }
        }

        if (matPos != std::string::npos)
        {
            size_t matStart = matPos + 5; // skip " mat="
            out.matFile = line.substr(matStart);
            while (!out.matFile.empty() && (out.matFile.back() == ' ' || out.matFile.back() == '\r' || out.matFile.back() == '\n'))
                out.matFile.pop_back();
        }

        return !out.file.empty();

    }

    // -----------------------------------------------------------------------
    // Recursively create ECS entities from parsed node records.
    // entityMap[nodeRecord.idx] = created node Entity.
    // -----------------------------------------------------------------------
    Entity SpawnNode(int                                              nodeIdx,
                     const std::vector<NodeRecord>&                   nodes,
                     // Per-F-line resolved data from MeshLibrary::GetEntry
                     const std::vector<Resource::Handle>&             fiLibHandle,
                     const std::vector<uint32_t>&                     fiMeshId,
                     const std::vector<uint32_t>&                     fiIndexCount,
                     const std::vector<uint32_t>&                     fiVertexCount,
                     const std::vector<DirectX::XMFLOAT3>&            fiAabbMin,
                     const std::vector<DirectX::XMFLOAT3>&            fiAabbMax,
                     const std::vector<std::string>&                  meshAbsPaths,
                     const std::vector<FileRecord>&                   fileRecords,
                     const std::filesystem::path&                     iscnDir,
                     World&                                           world,
                     Resource::AssetManager&                          assetMgr,
                     std::unordered_map<int, Entity>&                 entityMap,
                     std::unordered_map<std::string, MaterialComponent>& matCache,
                     uint32_t&                                        outNodeCount,
                     uint32_t&                                        outMeshCount,
                     std::unordered_map<int, std::vector<Entity>>*    meshFileToEntities = nullptr)
    {
        const NodeRecord& rec = nodes[nodeIdx];

        // ---- Node entity ---------------------------------------------------
        const Entity nodeEnt = world.CreateEntity();
        world.SetName(nodeEnt, rec.name.empty() ? "node" : rec.name);
        entityMap[rec.idx] = nodeEnt;
        ++outNodeCount;

        LocalTransform lt;
        lt.translation = { rec.tx, rec.ty, rec.tz };
        lt.rotation    = { rec.qx, rec.qy, rec.qz, rec.qw };
        lt.scale       = { rec.sx, rec.sy, rec.sz };

        world.AddComponent<LocalTransform> (nodeEnt, lt);
        world.AddComponent<GlobalTransform>(nodeEnt, GlobalTransform{});
        world.AddComponent<Visibility>     (nodeEnt, Visibility{});
        world.AddComponent<RenderLayer>    (nodeEnt, RenderLayer{});
        world.AddComponent<SceneNodeTag>   (nodeEnt, SceneNodeTag{});
        world.AddComponent<Children>       (nodeEnt, Children{});

        if (rec.parent >= 0)
        {
            auto it = entityMap.find(rec.parent);
            if (it != entityMap.end())
            {
                world.AddComponent<Parent>(nodeEnt, Parent{ it->second });
                if (Children* parentCh = world.GetComponent<Children>(it->second))
                    parentCh->entities.push_back(nodeEnt);
            }
        }

        // ---- One mesh entity per mesh file reference -----------------------
        for (int fi : rec.meshFileIndices)
        {
            if (fi < 0 || fi >= static_cast<int>(fiLibHandle.size())) continue;
            const Resource::Handle libH = fiLibHandle[fi];
            if (!libH.IsValid()) continue;

            const Entity meshEnt = world.CreateEntity();
            world.SetName(meshEnt, rec.name + "_mesh");
            ++outMeshCount;

            // New: MeshLibRef carries the library handle + meshId; the renderer
            // dereferences the library entry at draw time for the actual range.
            MeshLibRef ref;
            ref.libHandle = libH;
            ref.meshId    = fiMeshId[fi];

            world.AddComponent<LocalTransform> (meshEnt, LocalTransform{});
            world.AddComponent<GlobalTransform>(meshEnt, GlobalTransform{});
            world.AddComponent<MeshLibRef>     (meshEnt, ref);
            // Per-mesh AABB from the library entry. Any entry without a
            // computed AABB falls through as zero — harmless for rendering,
            // just means BVH will see a degenerate leaf there.
            WorldAabb meshAabb;
            meshAabb.min = fiAabbMin[fi];
            meshAabb.max = fiAabbMax[fi];
            world.AddComponent<LocalAabb>      (meshEnt, LocalAabb{ meshAabb.min, meshAabb.max });
            world.AddComponent<WorldAabb>      (meshEnt, meshAabb);
            world.AddComponent<Visibility>     (meshEnt, Visibility{});
            world.AddComponent<RenderLayer>    (meshEnt, RenderLayer{});
            world.AddComponent<Parent>         (meshEnt, Parent{ nodeEnt });
            // Load material from .imat if referenced in the F line, else default.
            // Bistro has 22k F-lines but only ~200 unique materials; without
            // this cache LoadMaterial parses each .imat file dozens of times,
            // which dominates scene-load time. Cache key is the absolute path
            // (already deduped by std::filesystem). Each entity still gets its
            // own copy of the MaterialComponent so per-entity edits don't bleed.
            if (fi >= 0 && fi < static_cast<int>(fileRecords.size()) && !fileRecords[fi].matFile.empty())
            {
                const std::string matAbsPath = (iscnDir / fileRecords[fi].matFile).string();
                auto it = matCache.find(matAbsPath);
                if (it == matCache.end())
                {
                    MaterialComponent loaded{};
                    const bool ok = Resource::LoadMaterial(matAbsPath, loaded);
                    auto inserted = matCache.emplace(matAbsPath,
                                                     ok ? std::move(loaded) : MaterialComponent{});
                    it = inserted.first;
                }
                world.AddComponent<MaterialComponent>(meshEnt, it->second);
                world.AddComponent<MaterialSourcePath>(meshEnt, MaterialSourcePath{ matAbsPath });
            }
            else
                world.AddComponent<MaterialComponent>(meshEnt, MaterialComponent{});

            if (fi >= 0 && fi < static_cast<int>(meshAbsPaths.size()) && !meshAbsPaths[fi].empty())
                world.AddComponent<MeshSourcePath>(meshEnt, MeshSourcePath{ meshAbsPaths[fi] });

            if (meshFileToEntities)
                (*meshFileToEntities)[fi].push_back(meshEnt);

            if (Children* nodeCh = world.GetComponent<Children>(nodeEnt))
                nodeCh->entities.push_back(meshEnt);
        }

        // ---- Recurse into children (nodes whose parent == rec.idx) ---------
        for (int ci = 0; ci < static_cast<int>(nodes.size()); ++ci)
        {
            if (nodes[ci].parent == rec.idx)
                SpawnNode(ci, nodes,
                          fiLibHandle, fiMeshId, fiIndexCount, fiVertexCount,
                          fiAabbMin, fiAabbMax,
                          meshAbsPaths, fileRecords, iscnDir,
                          world, assetMgr, entityMap, matCache,
                          outNodeCount, outMeshCount,
                          meshFileToEntities);
        }

        return nodeEnt;
    }

    // -----------------------------------------------------------------------
    // Per-mesh blend data entry stored after deserialization.
    // -----------------------------------------------------------------------
    struct IskelMeshBlend
    {
        uint32_t               meshIndex   = 0;
        std::vector<BlendVertex> blendData;
    };

    // -----------------------------------------------------------------------
    // Deserialized skeleton data (not registerd yet — caller registers).
    // skeleton is heap-allocated: SkeletonAsset is ~260 KB with MAX_BONES=1024
    // and must not live on the stack.
    // -----------------------------------------------------------------------
    struct IskelData
    {
        std::unique_ptr<SkeletonAsset> skeleton;
        std::vector<ClipAsset>         clips;
        std::vector<IskelMeshBlend>    meshBlends;
        bool valid = false;
    };

    // -----------------------------------------------------------------------
    // Load and deserialize a .iskel binary blob produced by SceneImporter.
    // -----------------------------------------------------------------------
    IskelData LoadIskel(const std::string& iskelPath)
    {
        IskelData out;

        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(iskelPath, blob))
        {
            LOG_ERROR("SceneInstanceLoader: cannot open .iskel '%s'", iskelPath.c_str());
            return out;
        }
        if (blob.empty()) return out;

        if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SKELETON))
        {
            LOG_ERROR("SceneInstanceLoader: bad .iskel header in '%s'", iskelPath.c_str());
            return out;
        }

        const Resource::SkeletonMetadata* meta =
            Resource::GetMetadata<Resource::SkeletonMetadata>(blob.data());
        const uint32_t boneCount       = meta->boneCount;
        const uint32_t clipCount       = meta->clipCount;
        const uint32_t meshBlendCount  = meta->reserved[0];

        if (boneCount > SkeletonAsset::MAX_BONES)
        {
            LOG_ERROR("SceneInstanceLoader: .iskel '%s' has %u bones but MAX_BONES=%u — re-export required",
                      iskelPath.c_str(), boneCount, SkeletonAsset::MAX_BONES);
            return out;
        }

        const uint8_t* src = Resource::GetPayload(blob.data());
        const uint8_t* data_end = blob.data() + blob.size();

        // meshBlendCount in payload
        uint32_t payloadMBC = 0;
        std::memcpy(&payloadMBC, src, sizeof(uint32_t)); src += sizeof(uint32_t);

        // Heap-allocate SkeletonAsset to avoid stack overflow (~260 KB with MAX_BONES=1024)
        out.skeleton = std::make_unique<SkeletonAsset>();
        SkeletonAsset& sk = *out.skeleton;
        sk.boneCount = boneCount;

        // parentIndex
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(&sk.parentIndex[i], src, sizeof(int32_t)); src += sizeof(int32_t); }

        // inverseBindPose
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(&sk.inverseBindPose[i], src, 16 * sizeof(float)); src += 16 * sizeof(float); }

        // bindPose
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(&sk.bindPose[i], src, 16 * sizeof(float)); src += 16 * sizeof(float); }

        // restPoseLocal
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(&sk.restPoseLocal[i], src, 16 * sizeof(float)); src += 16 * sizeof(float); }

        // boneNames + rebuild nameToIndex
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            std::memcpy(sk.boneNames[i], src, 64); src += 64;
            uint32_t hash = 2166136261u;
            for (const char* p = sk.boneNames[i]; *p; ++p)
                hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
            sk.nameToIndex[hash] = i;
        }

        // boneRestAABBs (6 floats per bone, may not exist in old .iskel files)
        if (src + boneCount * 6 * sizeof(float) <= data_end)
        {
            for (uint32_t i = 0; i < boneCount; ++i)
            {
                std::memcpy(&sk.boneRestAABBs[i].localMin, src, 3 * sizeof(float)); src += 3 * sizeof(float);
                std::memcpy(&sk.boneRestAABBs[i].localMax, src, 3 * sizeof(float)); src += 3 * sizeof(float);
            }
            sk.hasBoneAABBs = true;
        }

        // clips
        out.clips.resize(clipCount);
        for (uint32_t ci = 0; ci < clipCount; ++ci)
        {
            ClipAsset& clip = out.clips[ci];
            uint32_t bc = 0, fc = 0;
            std::memcpy(&bc,          src, sizeof(uint32_t)); src += sizeof(uint32_t);
            std::memcpy(&fc,          src, sizeof(uint32_t)); src += sizeof(uint32_t);
            std::memcpy(&clip.duration,  src, sizeof(float)); src += sizeof(float);
            std::memcpy(&clip.frameRate, src, sizeof(float)); src += sizeof(float);
            clip.boneCount  = bc;
            clip.frameCount = fc;
            const size_t kf = (size_t)bc * fc;
            clip.positions.resize(kf);
            clip.rotations.resize(kf);
            clip.scales.resize(kf);
            std::memcpy(clip.positions.data(), src, kf * 3 * sizeof(float)); src += kf * 3 * sizeof(float);
            std::memcpy(clip.rotations.data(), src, kf * 4 * sizeof(float)); src += kf * 4 * sizeof(float);
            std::memcpy(clip.scales.data(),    src, kf * 3 * sizeof(float)); src += kf * 3 * sizeof(float);
        }

        // per-mesh blend data
        out.meshBlends.resize(payloadMBC);
        for (uint32_t m = 0; m < payloadMBC; ++m)
        {
            uint32_t meshIdx = 0, vc = 0;
            std::memcpy(&meshIdx, src, sizeof(uint32_t)); src += sizeof(uint32_t);
            std::memcpy(&vc,      src, sizeof(uint32_t)); src += sizeof(uint32_t);
            out.meshBlends[m].meshIndex = meshIdx;
            out.meshBlends[m].blendData.resize(vc);
            std::memcpy(out.meshBlends[m].blendData.data(), src, vc * sizeof(BlendVertex));
            src += vc * sizeof(BlendVertex);
        }

        // Extended PMX data (ISKEL_FLAG_EXTENDED)
        const uint32_t iskelFlags = meta->reserved[1];
        if (iskelFlags & Resource::ISKEL_FLAG_EXTENDED)
        {
            // grantSource
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(&sk.grantSource[i], src, sizeof(int32_t)); src += sizeof(int32_t); }
            // grantRatio
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(&sk.grantRatio[i], src, sizeof(float)); src += sizeof(float); }
            // transformOrder
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(&sk.transformOrder[i], src, sizeof(int32_t)); src += sizeof(int32_t); }

            // IK chains
            uint32_t ikCount = 0;
            std::memcpy(&ikCount, src, sizeof(uint32_t)); src += sizeof(uint32_t);
            sk.ikChains.resize(ikCount);

            for (uint32_t ci = 0; ci < ikCount; ++ci)
            {
                auto& chain = sk.ikChains[ci];
                std::memcpy(&chain.ikBoneIndex,     src, sizeof(uint32_t)); src += sizeof(uint32_t);
                std::memcpy(&chain.targetBoneIndex, src, sizeof(uint32_t)); src += sizeof(uint32_t);
                std::memcpy(&chain.loopCount,       src, sizeof(uint32_t)); src += sizeof(uint32_t);
                std::memcpy(&chain.angleLimit,      src, sizeof(float));    src += sizeof(float);

                uint32_t linkCount = 0;
                std::memcpy(&linkCount, src, sizeof(uint32_t)); src += sizeof(uint32_t);
                chain.links.resize(linkCount);

                for (uint32_t li = 0; li < linkCount; ++li)
                {
                    auto& link = chain.links[li];
                    std::memcpy(&link.boneIndex, src, sizeof(uint32_t)); src += sizeof(uint32_t);
                    uint8_t hasLim = *src++;
                    link.hasAngleLimit = (hasLim != 0);
                    if (link.hasAngleLimit)
                    {
                        std::memcpy(&link.minAngle, src, sizeof(DirectX::XMFLOAT3)); src += sizeof(DirectX::XMFLOAT3);
                        std::memcpy(&link.maxAngle, src, sizeof(DirectX::XMFLOAT3)); src += sizeof(DirectX::XMFLOAT3);
                    }
                }
            }

            // Morph target names
            if (src + sizeof(uint32_t) <= data_end)
            {
                std::memcpy(&sk.morphTargetCount, src, sizeof(uint32_t)); src += sizeof(uint32_t);
                if (sk.morphTargetCount > SkeletonAsset::MAX_MORPH_TARGETS)
                    sk.morphTargetCount = SkeletonAsset::MAX_MORPH_TARGETS;
                for (uint32_t i = 0; i < sk.morphTargetCount; ++i)
                {
                    std::memcpy(sk.morphTargetNames[i], src, 64); src += 64;
                }
            }

            uint32_t grantCount = 0;
            for (uint32_t i = 0; i < boneCount; ++i)
                if (sk.grantSource[i] >= 0) ++grantCount;
            LOG_INFO("SceneInstanceLoader: extended data — %u grants, %u IK chains, %u morphTargets",
                     grantCount, ikCount, sk.morphTargetCount);
        }
        else
        {
            // Legacy .iskel: rebuild grant table from bone names
            Resource::SkeletonImporter::BuildGrantTable(sk);
        }

        out.valid = true;
        LOG_INFO("SceneInstanceLoader: .iskel '%s' — %u bones, %u clips, %u skinned meshes",
                 iskelPath.c_str(), boneCount, clipCount, payloadMBC);
        return out;
    }

    // -----------------------------------------------------------------------
    // Decompose a .imsh blob (PackedVertex layout) into separate streams for
    // skinned mesh registration.
    // -----------------------------------------------------------------------
    struct ImshStreams
    {
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT3> normals;
        std::vector<DirectX::XMFLOAT2> uvs;
        std::vector<uint32_t>           indices;
        bool valid = false;
    };



    ImshStreams DecomposeImsh(const std::string& imshPath)
    {
        ImshStreams out;
        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(imshPath, blob)) return out;
        if (blob.empty()) return out;

        if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESH))
            return out;

        const Resource::MeshMetadata* meta =
            Resource::GetMetadata<Resource::MeshMetadata>(blob.data());
        const uint32_t nv = meta->vertexCount;
        const uint32_t ni = meta->indexCount;

        const uint8_t* payload = Resource::GetPayload(blob.data());
        const auto* verts      = reinterpret_cast<const PackedVertex*>(payload);
        const auto* idxData    = reinterpret_cast<const uint32_t*>(payload + nv * sizeof(PackedVertex));

        out.positions.resize(nv);
        out.normals.resize(nv);
        out.uvs.resize(nv);
        for (uint32_t i = 0; i < nv; ++i)
        {
            out.positions[i] = { verts[i].px, verts[i].py, verts[i].pz };
            out.normals[i]   = { verts[i].nx, verts[i].ny, verts[i].nz };
            out.uvs[i]       = { verts[i].u,  verts[i].v  };
        }
        out.indices.assign(idxData, idxData + ni);
        out.valid = true;
        return out;
    }

    // Extract the vertex/index slice for a single MeshLibraryEntry from a
    // .meshlib file. Indices are re-based into the local [0, vertexCount)
    // range so the returned streams are self-contained (match the .imsh
    // semantics expected by SkinnedMeshPending).
    ImshStreams DecomposeMeshLibEntry(const std::string& meshlibPath, uint32_t meshId)
    {
        ImshStreams out;
        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(meshlibPath, blob)) return out;
        if (blob.empty()) return out;

        if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESHLIB))
            return out;

        const auto* meta = Resource::GetMetadata<Resource::MeshLibraryMetadata>(blob.data());
        if (meshId >= meta->meshCount) return out;

        const uint8_t* payload   = Resource::GetPayload(blob.data());
        const auto*    entries   = reinterpret_cast<const Resource::MeshLibraryEntry*>(payload);
        const auto&    entry     = entries[meshId];

        // Stride-aware pointer math — must use the on-disk vertexStride
        // (32 for legacy, 48 for with-tangent assets) rather than
        // sizeof(PackedVertex) so we land on the correct byte for each
        // vertex's pos/normal/uv triplet regardless of whether tangents
        // follow it in memory.
        const uint32_t vStride   = meta->vertexStride;
        const uint8_t* vbBase    = payload + meta->meshCount * sizeof(Resource::MeshLibraryEntry);
        const uint8_t* ibBase    = vbBase + size_t(meta->vertexCount) * vStride;
        const uint8_t* vertsBase = vbBase + size_t(entry.vertexStart) * vStride;
        const auto*    idxData   = reinterpret_cast<const uint32_t*>(ibBase) + entry.indexStart;

        out.positions.resize(entry.vertexCount);
        out.normals.resize(entry.vertexCount);
        out.uvs.resize(entry.vertexCount);
        for (uint32_t i = 0; i < entry.vertexCount; ++i)
        {
            const auto* v = reinterpret_cast<const PackedVertex*>(vertsBase + size_t(i) * vStride);
            out.positions[i] = { v->px, v->py, v->pz };
            out.normals[i]   = { v->nx, v->ny, v->nz };
            out.uvs[i]       = { v->u,  v->v  };
        }
        // Re-base indices into local [0, vertexCount) — the meshlib stores them
        // pre-offset into the shared VB; SkinnedMeshPending expects local indices.
        out.indices.resize(entry.indexCount);
        for (uint32_t i = 0; i < entry.indexCount; ++i)
            out.indices[i] = idxData[i] - entry.vertexStart;
        out.valid = true;
        return out;
    }

    // File-extension dispatcher: picks the right decoder so the skinning code
    // below doesn't have to care whether the source was a legacy .imsh or a
    // meshlib entry.
    ImshStreams DecomposeMeshSource(const std::string& path, uint32_t meshId)
    {
        const auto dot = path.find_last_of('.');
        if (dot != std::string::npos)
        {
            std::string ext = path.substr(dot);
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".meshlib") return DecomposeMeshLibEntry(path, meshId);
        }
        return DecomposeImsh(path);
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
// SceneInstanceLoader::Load
// ---------------------------------------------------------------------------
SceneInstanceLoader::LoadResult SceneInstanceLoader::Load(const std::string&      iscnPath,
                                                           World&                  world,
                                                           Resource::AssetManager& assetMgr,
                                                           Resource::MeshLibrary&  meshLib,
                                                           Renderer*               renderer)
{
    LoadResult result;

    IGraphicsDevice* gfxPtr = assetMgr.GetGraphicsDevice();
    if (!gfxPtr)
    {
        LOG_ERROR("SceneInstanceLoader: AssetManager has no graphics device; cannot load '%s'",
                  iscnPath.c_str());
        return result;
    }
    IGraphicsDevice& gfx = *gfxPtr;

    // ---- Read .iscn blob ---------------------------------------------------
    std::vector<uint8_t> blob;
    if (!::Resource::AssetFS::Get().ReadFile(iscnPath, blob))
    {
        LOG_ERROR("SceneInstanceLoader: cannot open '%s'", iscnPath.c_str());
        return result;
    }
    if (blob.empty()) return result;

    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
    {
        LOG_ERROR("SceneInstanceLoader: bad .iscn header in '%s'", iscnPath.c_str());
        return result;
    }

    // Text payload (null-terminated)
    const char* text = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));

    // ---- Parse lines -------------------------------------------------------
    std::vector<NodeRecord> nodes;
    std::vector<FileRecord> fileRecords;
    std::string skelRelFile;   // bare filename from K line (empty = no skeleton)

    // M-line: per-mesh morph target files. Key = mesh file index, value = .imorph filename.
    struct MorphFileRef { int meshFileIdx; std::string file; };
    std::vector<MorphFileRef> morphFileRefs;

    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.size() < 2) continue;
        if (line[0] == '#') continue;

        if (line[0] == 'N' && line[1] == ' ')
        {
            NodeRecord nr;
            if (ParseNLine(line, nr))
                nodes.push_back(std::move(nr));
        }
        else if (line[0] == 'F' && line[1] == ' ')
        {
            FileRecord fr;
            if (ParseFLine(line, fr))
                fileRecords.push_back(std::move(fr));
        }
        else if (line[0] == 'K' && line[1] == ' ')
        {
            ParseKLine(line, skelRelFile);
        }
        else if (line[0] == 'M' && line[1] == ' ')
        {
            // M idx=<meshFileIdx> file=<morphFileName>
            MorphFileRef mref;
            mref.meshFileIdx = -1;
            auto idxPos = line.find("idx=");
            auto filePos = line.find("file=");
            if (idxPos != std::string::npos)
                mref.meshFileIdx = std::stoi(line.substr(idxPos + 4));
            if (filePos != std::string::npos)
                mref.file = line.substr(filePos + 5);
            // Trim trailing whitespace
            while (!mref.file.empty() && (mref.file.back() == ' ' || mref.file.back() == '\r' || mref.file.back() == '\n'))
                mref.file.pop_back();
            if (mref.meshFileIdx >= 0 && !mref.file.empty())
                morphFileRefs.push_back(std::move(mref));
        }
    }

    if (nodes.empty())
    {
        LOG_ERROR("SceneInstanceLoader: no nodes found in '%s'", iscnPath.c_str());
        return result;
    }

    LOG_INFO("SceneInstanceLoader: '%s' — %zu nodes, %zu mesh files",
             iscnPath.c_str(), nodes.size(), fileRecords.size());

    // ---- Acquire mesh libraries (new P1-P6 path) ---------------------------
    // Each distinct `file=` referenced by an F-line is one .meshlib file. The
    // library is loaded once, uploaded as a single shared VB + IB, and all
    // F-lines sharing the same file look up their mesh by meshId. This
    // replaces the legacy AcquireMeshBatch + ImshPack + SubMeshRecord path.
    const fs::path iscnDir = fs::path(iscnPath).parent_path();

    std::unordered_map<std::string, Resource::Handle> libByPath;
    libByPath.reserve(8);

    // Per-F-line resolved data: which library + which meshId + AABB + counts.
    std::vector<Resource::Handle>  fiLibHandle(fileRecords.size());
    std::vector<uint32_t>          fiMeshId      (fileRecords.size(), 0u);
    std::vector<DirectX::XMFLOAT3> fiAabbMin     (fileRecords.size(), DirectX::XMFLOAT3{0,0,0});
    std::vector<DirectX::XMFLOAT3> fiAabbMax     (fileRecords.size(), DirectX::XMFLOAT3{0,0,0});
    std::vector<uint32_t>          fiIndexCount  (fileRecords.size(), 0u);
    std::vector<uint32_t>          fiVertexCount (fileRecords.size(), 0u);
    std::vector<std::string>       meshAbsPaths  (fileRecords.size());

    uint32_t failedCount = 0;
    for (int fi = 0; fi < static_cast<int>(fileRecords.size()); ++fi)
    {
        const auto& fr       = fileRecords[fi];
        meshAbsPaths[fi]     = (iscnDir / fr.file).string();

        auto it = libByPath.find(meshAbsPaths[fi]);
        Resource::Handle libH;
        if (it != libByPath.end())
        {
            libH = it->second;
        }
        else
        {
            libH = meshLib.Load(meshAbsPaths[fi], gfx);
            libByPath.emplace(meshAbsPaths[fi], libH);
        }

        if (!libH.IsValid())
        {
            ++failedCount;
            continue;
        }

        const auto* entry = meshLib.GetEntry(libH, fr.subIdx);
        if (!entry)
        {
            LOG_WARNING("SceneInstanceLoader: F[%d] meshId=%u out of range in '%s'",
                        fi, fr.subIdx, fr.file.c_str());
            ++failedCount;
            continue;
        }

        fiLibHandle[fi]   = libH;
        fiMeshId[fi]      = fr.subIdx;
        fiAabbMin[fi]     = entry->aabbMin;
        fiAabbMax[fi]     = entry->aabbMax;
        fiIndexCount[fi]  = entry->indexCount;
        fiVertexCount[fi] = entry->vertexCount;
    }
    if (failedCount > 0)
        LOG_WARNING("SceneInstanceLoader: %u/%zu mesh refs failed to resolve",
                    failedCount, fileRecords.size());

    // ---- Build ECS hierarchy (DFS from root nodes) -------------------------
    // Find roots: nodes whose parent == -1.
    std::unordered_map<int, Entity> entityMap;
    entityMap.reserve(nodes.size() * 2);

    // meshFileIdx -> all mesh entities created for that file
    const bool needSkelMap = (!skelRelFile.empty() && renderer != nullptr);
    std::unordered_map<int, std::vector<Entity>> meshFileToEntities;

    uint32_t nodeCount = 0, meshCount = 0;
    Entity rootEntity = NullEntity;

    // Sort nodes by idx so parent entities are always created first.
    std::sort(nodes.begin(), nodes.end(),
              [](const NodeRecord& a, const NodeRecord& b){ return a.idx < b.idx; });

    // Material cache shared across the whole scene tree — see SpawnNode body
    // for why this lives at Load scope rather than per-node scope.
    std::unordered_map<std::string, MaterialComponent> matCache;
    matCache.reserve(256);

    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
        if (nodes[i].parent == -1)
        {
            Entity e = SpawnNode(i, nodes,
                                 fiLibHandle, fiMeshId, fiIndexCount, fiVertexCount,
                                 fiAabbMin, fiAabbMax,
                                 meshAbsPaths, fileRecords, iscnDir,
                                 world, assetMgr, entityMap, matCache,
                                 nodeCount, meshCount,
                                 needSkelMap ? &meshFileToEntities : nullptr);
            if (rootEntity == NullEntity)
                rootEntity = e;
        }
    }

    // ---- Skeleton / skinned mesh registration --------------------------------
    if (needSkelMap)
    {
        const std::string iskelPath = (iscnDir / skelRelFile).string();
        IskelData iskel = LoadIskel(iskelPath);

        if (iskel.valid && !iskel.meshBlends.empty())
        {
            // Register skeleton into renderer
            const uint32_t skelIdx =
                renderer->GetSkeletonRegistry().Register(std::move(*iskel.skeleton));

            // Register all clips
            for (auto& clip : iskel.clips)
                renderer->GetClipLibrary().Register(std::move(clip));

            LOG_INFO("SceneInstanceLoader: registered skeleton idx=%u, %zu clips",
                     skelIdx, iskel.clips.size());

            // For each skinned mesh, build pending and register GPU buffers
            for (const IskelMeshBlend& mb : iskel.meshBlends)
            {
                const int fi = static_cast<int>(mb.meshIndex);
                auto it = meshFileToEntities.find(fi);
                if (it == meshFileToEntities.end() || it->second.empty())
                {
                    LOG_WARNING("SceneInstanceLoader: no entity for skinned meshFileIdx=%d", fi);
                    continue;
                }
                if (fi >= static_cast<int>(meshAbsPaths.size()) || meshAbsPaths[fi].empty())
                {
                    LOG_WARNING("SceneInstanceLoader: no mesh source path for meshFileIdx=%d", fi);
                    continue;
                }

                // DecomposeMeshSource auto-detects .meshlib vs .imsh. For meshlib
                // it uses the F-line's meshId (fiMeshId[fi]) to slice out the
                // right entry's vertex/index range with local-rebased indices.
                ImshStreams streams = DecomposeMeshSource(meshAbsPaths[fi], fiMeshId[fi]);
                if (!streams.valid)
                {
                    LOG_WARNING("SceneInstanceLoader: failed to decompose mesh source for idx=%d path='%s'",
                                fi, meshAbsPaths[fi].c_str());
                    continue;
                }

                // Validate vertex count match between mesh source and .iskel blend data
                const uint32_t imshVerts  = static_cast<uint32_t>(streams.positions.size());
                const uint32_t blendVerts = static_cast<uint32_t>(mb.blendData.size());
                if (imshVerts != blendVerts)
                {
                    LOG_ERROR("SceneInstanceLoader: VERTEX COUNT MISMATCH! "
                              "meshFileIdx=%d mesh=%u src=%u verts .iskel blend=%u verts — skinning will be WRONG",
                              fi, fiMeshId[fi], imshVerts, blendVerts);
                }

                // Use the first entity for this mesh file (typically only one)
                for (Entity meshEnt : it->second)
                {
                    SceneLoader::SkinnedMeshPending pending;
                    pending.entity        = meshEnt;
                    pending.rootEntity    = rootEntity;
                    pending.skeletonIndex = skelIdx;
                    pending.restPositions = streams.positions;
                    pending.restNormals   = streams.normals;
                    pending.uvs           = streams.uvs;
                    pending.indices       = streams.indices;
                    pending.blendData     = mb.blendData;

                    // Load .imorph if available for this mesh
                    for (const auto& mref : morphFileRefs)
                    {
                        if (mref.meshFileIdx == fi)
                        {
                            const std::string imorphPath = (iscnDir / mref.file).string();
                            std::vector<uint8_t> mblob;
                            if (::Resource::AssetFS::Get().ReadFile(imorphPath, mblob) && !mblob.empty())
                            {
                                if (Resource::ValidateHeader(mblob.data(), mblob.size(), Resource::MAGIC_MORPH))
                                {
                                    const auto* mmeta = Resource::GetMetadata<Resource::MorphTargetMetadata>(mblob.data());
                                    const uint8_t* payload = Resource::GetPayload(mblob.data());

                                    pending.morphCount = mmeta->morphTargetCount;
                                    pending.morphDeltas.resize(
                                        static_cast<size_t>(mmeta->morphTargetCount) * mmeta->vertexCount);
                                    pending.morphNames.resize(mmeta->morphTargetCount);

                                    const uint8_t* src = payload;
                                    for (uint32_t mi = 0; mi < mmeta->morphTargetCount; ++mi)
                                    {
                                        char name[64];
                                        std::memcpy(name, src, 64); src += 64;
                                        pending.morphNames[mi] = name;

                                        const size_t deltaBytes = mmeta->vertexCount * sizeof(DirectX::XMFLOAT3);
                                        std::memcpy(&pending.morphDeltas[static_cast<size_t>(mi) * mmeta->vertexCount],
                                                    src, deltaBytes);
                                        src += deltaBytes;
                                    }
                                    LOG_INFO("SceneInstanceLoader:   .imorph '%s' — %u morphs, %u verts",
                                             mref.file.c_str(), mmeta->morphTargetCount, mmeta->vertexCount);
                                }
                            }
                            break;
                        }
                    }

                    LOG_INFO("SceneInstanceLoader:   meshFileIdx=%d entity=%u verts=%u blend=%u morphs=%u",
                             fi, meshEnt, imshVerts, blendVerts, pending.morphCount);

                    if (!renderer->RegisterSkinnedMeshFull(world, pending))
                        LOG_WARNING("SceneInstanceLoader: RegisterSkinnedMeshFull failed for entity=%u", meshEnt);
                }
            }
        }
        else if (iskel.valid)
        {
            LOG_INFO("SceneInstanceLoader: .iskel has no blend data — static skeleton only");
        }
    }

    LOG_INFO("SceneInstanceLoader: material cache: %zu unique .imat loaded for %u mesh entities",
             matCache.size(), meshCount);

    // Store .iscn path on root entity for prefab serialization
    if (rootEntity != NullEntity)
        world.AddComponent<SceneSourcePath>(rootEntity, SceneSourcePath{ iscnPath });

    result.rootEntity      = rootEntity;
    result.nodeCount       = nodeCount;
    result.meshEntityCount = meshCount;
    result.success         = (rootEntity != NullEntity);

    LOG_INFO("SceneInstanceLoader: done — %u node entities, %u mesh entities, root=%u",
             nodeCount, meshCount, rootEntity);

    return result;
}

