#include "Resource/WorldSerializer.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetManager.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceHandle.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/PostProcessConfig.h"
#include "System/TaskSystem.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/PhysicsComponents.h"   // ColliderComponent / RigidBodyComponent (MC override lines)
#include "Scene/SceneInstanceLoader.h"
#include "Graphics/Renderer.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace DirectX;
using Resource::AnimHandle;

namespace
{

std::string PercentEncode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s)
    {
        if (c == '%')       r += "%25";
        else if (c == ' ')  r += "%20";
        else                r += c;
    }
    return r;
}

std::string PercentDecode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int h = hex(s[i+1]), l = hex(s[i+2]);
            if (h >= 0 && l >= 0) { r += static_cast<char>(h*16+l); i += 2; continue; }
        }
        r += s[i];
    }
    return r;
}

// Collect ALL entities in the world, building a parent-child map.
struct NodeInfo { int idx; int parentIdx; Entity entity; };

void CollectAllEntities(World& world,
                        std::unordered_map<Entity, int>& entityToIdx,
                        std::vector<NodeInfo>& out)
{
    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;
        int idx = static_cast<int>(out.size());
        entityToIdx[e] = idx;
        out.push_back({ idx, -1, e });
    }
    for (auto& ni : out)
    {
        const Parent* p = world.GetComponent<Parent>(ni.entity);
        if (p && p->entity != NullEntity)
        {
            auto it = entityToIdx.find(p->entity);
            if (it != entityToIdx.end())
                ni.parentIdx = it->second;
        }
    }
}

// Apply MaterialOverride property bag onto a MaterialComponent.
void ApplyMaterialOverride(MaterialComponent& mat, const MaterialOverride& ovr)
{
    using V = MaterialOverride::Value;
    for (const auto& [key, val] : ovr.props)
    {
        if (val.type == V::Vec4)
        {
            XMFLOAT4 v = { val.data.v4[0], val.data.v4[1], val.data.v4[2], val.data.v4[3] };
            if      (key == "baseColor")     mat.baseColor = v;
            else if (key == "specularColor") mat.specularColor = v;
            else if (key == "emissiveColor") mat.emissiveColor = v;
        }
        else if (val.type == V::Float)
        {
            if      (key == "roughnessMax")     mat.roughnessMax = val.data.f;
            else if (key == "roughnessMin")     mat.roughnessMin = val.data.f;
            else if (key == "metalnessMax")     mat.metalnessMax = val.data.f;
            else if (key == "metalnessMin")     mat.metalnessMin = val.data.f;
            else if (key == "reflectance")      mat.reflectance = val.data.f;
            else if (key == "normalMapStrength")mat.normalMapStrength = val.data.f;
            else if (key == "saturation")       mat.saturation = val.data.f;
            else if (key == "alphaRef")         mat.alphaRef = val.data.f;
            else if (key == "outlinePixels")    mat.outlinePixels = val.data.f;
        }
        else if (val.type == V::Int)
        {
            if      (key == "blendMode")  mat.userBlendMode = static_cast<BlendMode>(val.data.i);
            else if (key == "shaderType") mat.shaderType = static_cast<MaterialComponent::SHADERTYPE>(val.data.i);
        }
        else if (val.type == V::TexRef && !val.texPath.empty())
        {
            if      (key == "tex_BASECOLORMAP") mat.textures[MaterialComponent::BASECOLORMAP].name = val.texPath;
            else if (key == "tex_NORMALMAP")    mat.textures[MaterialComponent::NORMALMAP].name = val.texPath;
            else if (key == "tex_SURFACEMAP")   mat.textures[MaterialComponent::SURFACEMAP].name = val.texPath;
            else if (key == "tex_EMISSIVEMAP")  mat.textures[MaterialComponent::EMISSIVEMAP].name = val.texPath;
        }
    }
}

// Parse N line → node record.
struct NodeRecord
{
    int   idx    { -1 };
    int   parent { -1 };
    float tx{0}, ty{0}, tz{0};
    float qx{0}, qy{0}, qz{0}, qw{1};
    float sx{1}, sy{1}, sz{1};
    std::string name;
};

bool ParseNLine(const std::string& line, NodeRecord& out)
{
    std::istringstream ss(line.substr(2));
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
            else if (key == "tx") out.tx = std::stof(val); else if (key == "ty") out.ty = std::stof(val);
            else if (key == "tz") out.tz = std::stof(val);
            else if (key == "qx") out.qx = std::stof(val); else if (key == "qy") out.qy = std::stof(val);
            else if (key == "qz") out.qz = std::stof(val); else if (key == "qw") out.qw = std::stof(val);
            else if (key == "sx") out.sx = std::stof(val); else if (key == "sy") out.sy = std::stof(val);
            else if (key == "sz") out.sz = std::stof(val);
            else if (key == "name") out.name = PercentDecode(val);
        }
        catch (...) {}
    }
    return out.idx >= 0;
}

// Parse indented component line → tag + KVMap.
struct ComponentBlock { int nodeIdx; std::string tag; KVMap kv; };

bool ParseComponentLine(const std::string& line, int currentNodeIdx, ComponentBlock& out)
{
    if (line.size() < 4 || line[0] != ' ' || line[1] != ' ') return false;
    auto colonPos = line.find(':', 2);
    if (colonPos == std::string::npos) return false;

    out.nodeIdx = currentNodeIdx;
    out.tag     = line.substr(2, colonPos - 2);
    out.kv.clear();

    size_t start = colonPos + 1;
    if (start < line.size() && line[start] == ' ') ++start;

    std::istringstream ss(line.substr(start));
    std::string token;
    while (ss >> token)
    {
        auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        out.kv[token.substr(0, eq)] = token.substr(eq + 1);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Phase 1 / Phase 2 pre-load helpers (SceneLoading_ResourceManager v2 §5.3, §8.1)
// ---------------------------------------------------------------------------
//
// LoadWorld used to Clear() the world *before* deserialize ran, so any texture
// or anim shared with the previous world fell out of the system caches and was
// reloaded from disk during the first frames after load. The new flow kicks
// off async I/O for every loader-backed path *before* Clear(), waits for the
// RM slots to reach Ready, then clears + deserializes. Path-hash dedup keeps
// the common subset warm across the swap; new resources finish loading on
// worker threads while we're still rendering the old world from the editor
// UI thread.
//
// Only paths whose extension has a registered IResourceLoader can be batched
// here. Today that's textures (MaterialOverride 't:'-typed values) and anim
// clips (AnimRef.path). .iscn / .meshlib / .imsh stay on the synchronous
// path inside SceneInstanceLoader / MeshLibrary — pre-loading those needs
// loaders to be registered first.

struct PreloadEntry
{
    std::string            path;
    Resource::ResourceType type;
};

std::vector<PreloadEntry> CollectPreloadPaths(const std::vector<ComponentBlock>& compBlocks)
{
    std::vector<PreloadEntry> out;
    std::unordered_set<uint64_t> seen;

    auto Add = [&](std::string path, Resource::ResourceType type) {
        if (path.empty()) return;
        // FNV-1a over normalized-ish path — RM does its own normalization on Load,
        // we only need dedup within this single LoadWorld call.
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : path) { h ^= c; h *= 1099511628211ULL; }
        if (!seen.insert(h).second) return;
        out.push_back({ std::move(path), type });
    };

    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "MaterialOverride")
        {
            for (const auto& [rawKey, rawVal] : cb.kv)
            {
                if (rawVal.size() < 2 || rawVal[0] != 't' || rawVal[1] != ':') continue;
                Add(PercentDecode(rawVal.substr(2)), Resource::ResourceType::Texture);
            }
        }
        else if (cb.tag == "AnimRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
                Add(PercentDecode(it->second), Resource::ResourceType::Animation);
        }
    }
    return out;
}

// Collect .iscn paths (SceneRef components) for fire-and-forget OS page-cache
// warmup. SceneInstanceLoader::Load still reads sync after Clear, but the
// kernel page cache has the .iscn bytes hot from the warmup, so the
// stdio/AssetFS read inside SceneInstanceLoader returns from RAM instead of
// disk. No RM integration — these don't have IResourceLoaders registered.
std::vector<std::string> CollectScenePaths(const std::vector<ComponentBlock>& compBlocks)
{
    std::vector<std::string> out;
    std::unordered_set<uint64_t> seen;
    for (const auto& cb : compBlocks)
    {
        if (cb.tag != "SceneRef") continue;
        auto it = cb.kv.find("path");
        if (it == cb.kv.end()) continue;
        std::string path = PercentDecode(it->second);
        if (path.empty()) continue;
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : path) { h ^= c; h *= 1099511628211ULL; }
        if (!seen.insert(h).second) continue;
        out.push_back(std::move(path));
    }
    return out;
}

// Recursive page-cache warmup for one .iscn — opens the file on a worker,
// parses out the sibling references (.meshlib via F file=, .imat via F mat=,
// .iskel via K file=, .imorph via M file=) and pushes a secondary read job
// per unique sibling. By the time SceneInstanceLoader's sync read kicks in,
// every file it touches is sitting in the OS page cache.
//
// Why no RM integration: these extensions have no registered IResourceLoader,
// so rm->Load can't drive them. The bytes here are discarded — sole purpose
// is hinting the kernel cache. Cost is bounded by .iscn text size (≤ few
// hundred KB even for Bistro) + N small worker reads.
void EnqueueIscnRecursiveWarmup(const std::string& scenePath)
{
    TaskSystem::Get().Push([scenePath]()
    {
        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(scenePath, blob)) return;
        if (blob.empty()) return;
        if (!::Resource::ValidateHeader(blob.data(), blob.size(),
                                        ::Resource::MAGIC_SCENE)) return;

        const char* text = reinterpret_cast<const char*>(
            ::Resource::GetPayload(blob.data()));
        const std::filesystem::path sceneDir =
            std::filesystem::path(scenePath).parent_path();

        std::unordered_set<uint64_t> seen;  // dedup per .iscn
        uint32_t enqueued = 0;
        auto pushWarmup = [&seen, &sceneDir, &enqueued](std::string filename) {
            // Trim trailing CR/LF/space (line.find returns up-to-EOL substring).
            while (!filename.empty() &&
                   (filename.back() == '\r' || filename.back() == '\n' ||
                    filename.back() == ' '))
                filename.pop_back();
            if (filename.empty()) return;

            std::string absPath = (sceneDir / filename).string();
            uint64_t h = 14695981039346656037ULL;
            for (unsigned char c : absPath) { h ^= c; h *= 1099511628211ULL; }
            if (!seen.insert(h).second) return;

            ++enqueued;
            TaskSystem::Get().Push([absPath]() {
                std::vector<uint8_t> b;
                ::Resource::AssetFS::Get().ReadFile(absPath, b);
                // bytes discarded — sole purpose is OS page-cache warmup
            });
        };

        // Extract a `key=` token's value up to the next space (or EOL).
        auto extractToken = [](const std::string& line, const char* key) -> std::string {
            const size_t kpos = line.find(key);
            if (kpos == std::string::npos) return {};
            const size_t start = kpos + std::strlen(key);
            const size_t end   = line.find(' ', start);
            return (end == std::string::npos)
                ? line.substr(start)
                : line.substr(start, end - start);
        };

        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line))
        {
            if (line.size() < 2 || line[0] == '#') continue;

            // F idx=I file=<name.meshlib|name.imsh> mesh=M [mat=<name.imat>]
            if (line[0] == 'F' && line[1] == ' ')
            {
                pushWarmup(extractToken(line, "file="));
                pushWarmup(extractToken(line, "mat="));
            }
            // K file=<name.iskel>
            else if (line[0] == 'K' && line[1] == ' ')
            {
                pushWarmup(extractToken(line, "file="));
            }
            // M idx=I file=<name.imorph>
            else if (line[0] == 'M' && line[1] == ' ')
            {
                pushWarmup(extractToken(line, "file="));
            }
        }

        LOG_INFO("WorldSerializer: .iscn warmup '%s' queued %u sibling reads",
                 scenePath.c_str(), enqueued);
    });
}

void WaitForHandlesReady(Resource::ResourceManager& rm,
                         const std::vector<Resource::Handle>& handles)
{
    using namespace std::chrono_literals;
    if (handles.empty()) return;

    // Main-thread GPU upload pump must run inside the wait — workers can finish
    // file I/O but the final ID3D12Resource upload happens here, so without a
    // pump we'd deadlock on textures stuck at Loading.
    while (true)
    {
        bool anyLoading = false;
        for (Resource::Handle h : handles)
        {
            if (rm.GetState(h) == Resource::ResourceState::Loading)
            {
                anyLoading = true;
                break;
            }
        }
        rm.ProcessPendingGPUUploads(2.0f);
        if (!anyLoading) break;
        std::this_thread::sleep_for(1ms);
    }
}

} // anonymous namespace

// ===========================================================================
// SaveWorld — generic component serialization via ComponentSerializerRegistry
// ===========================================================================
bool Resource::SaveWorld(World& world, const std::string& path,
                          const std::string& sceneName,
                          const std::string& postProcessConfigPath,
                          const std::string& navMeshPath)
{
    auto& reg = GetComponentRegistry();

    std::unordered_map<Entity, int> entityToIdx;
    std::vector<NodeInfo> nodes;
    CollectAllEntities(world, entityToIdx, nodes);

    if (nodes.empty())
    {
        LOG_WARNING("WorldSerializer: world is empty, nothing to save");
        return false;
    }

    // Identify skinned entity subtrees: only the root gets serialized.
    // Children come from the .iscn on load, so skip them to avoid duplicates.
    std::unordered_set<Entity> skinnedSubtree;
    {
        std::function<void(Entity)> markChildren = [&](Entity parent) {
            const Children* ch = world.GetComponent<Children>(parent);
            if (!ch) return;
            for (Entity child : ch->entities)
            {
                skinnedSubtree.insert(child);
                markChildren(child);
            }
        };
        for (const NodeInfo& ni : nodes)
        {
            const SceneSourcePath* ssp = world.GetComponent<SceneSourcePath>(ni.entity);
            if (ssp && !ssp->path.empty())
                markChildren(ni.entity);
        }
    }

    std::ostringstream ss;
    ss << "# DX12 Engine World Scene\n";
    ss << "W name=" << PercentEncode(sceneName);
    if (!postProcessConfigPath.empty())
        ss << " postProcessConfig=" << PercentEncode(postProcessConfigPath);
    if (!navMeshPath.empty())
        ss << " navMesh=" << PercentEncode(navMeshPath);
    ss << "\n";

    char buf[512];

    for (const NodeInfo& ni : nodes)
    {
        const Entity e = ni.entity;
        if (skinnedSubtree.count(e)) continue;

        // --- N line: transform + name ---
        const LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        float tx=0,ty=0,tz=0, qx=0,qy=0,qz=0,qw=1, sx=1,sy=1,sz=1;
        if (lt)
        {
            tx = lt->translation.x; ty = lt->translation.y; tz = lt->translation.z;
            qx = lt->rotation.x;    qy = lt->rotation.y;    qz = lt->rotation.z; qw = lt->rotation.w;
            sx = lt->scale.x;       sy = lt->scale.y;       sz = lt->scale.z;
        }

        snprintf(buf, sizeof(buf),
            "N idx=%d parent=%d tx=%.6f ty=%.6f tz=%.6f"
            " qx=%.6f qy=%.6f qz=%.6f qw=%.6f sx=%.6f sy=%.6f sz=%.6f",
            ni.idx, ni.parentIdx, tx, ty, tz, qx, qy, qz, qw, sx, sy, sz);
        ss << buf << " name=" << PercentEncode(world.GetName(e)) << "\n";

        // --- Pre-step: auto-create .imat if needed ---
        if (world.HasComponent<MaterialComponent>(e) && !world.HasComponent<MaterialSourcePath>(e))
        {
            const MaterialComponent* mat = world.GetComponent<MaterialComponent>(e);
            namespace fs = std::filesystem;
            const fs::path worldDir = fs::path(path).parent_path();
            const fs::path matDir   = worldDir / "Materials";
            fs::create_directories(matDir);

            std::string entName = world.GetName(e);
            if (entName.empty()) entName = "entity";
            std::string safeName;
            for (char c : entName)
                safeName += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
            std::string matFileName = safeName + "_mat" + std::to_string(ni.idx) + ".imat";
            std::string matAbsPath  = (matDir / matFileName).string();

            SaveMaterial(*mat, matAbsPath);

            if (!world.HasComponent<MaterialSourcePath>(e))
                world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ matAbsPath });
            else
                world.GetComponent<MaterialSourcePath>(e)->path = matAbsPath;
        }

        // --- Generic component serialization via registry ---
        for (const auto& [typeIdx, serializer] : reg.All())
        {
            if (serializer.has(world, e))
                serializer.serialize(world, e, ss);
        }
    }

    // --- Mesh-collider overrides for skinnedSubtree entities --------------
    // Scene-tree entities (everything under a SceneSourcePath) are skipped
    // above because the .iscn re-spawns them on load — we'd duplicate
    // otherwise. But ColliderComponent/RigidBodyComponent attached via the
    // editor "Use baked as collision" pass DON'T live in the .iscn. They're
    // overrides keyed by (sourcePath, sourceMeshId), so save one MC line per
    // unique key and replay on load.
    {
        struct MCEntry { ColliderComponent col; RigidBodyComponent rb; bool hasCol=false; bool hasRb=false; };
        std::unordered_map<std::string, MCEntry> mc;
        for (const NodeInfo& ni : nodes)
        {
            const Entity e = ni.entity;
            if (!skinnedSubtree.count(e)) continue;

            const ColliderComponent*  col = world.GetComponent<ColliderComponent>(e);
            const RigidBodyComponent* rb  = world.GetComponent<RigidBodyComponent>(e);
            if (!col && !rb) continue;

            const MeshLibRef*      mlr = world.GetComponent<MeshLibRef>(e);
            const MeshSourcePath*  sp  = world.GetComponent<MeshSourcePath>(e);
            if (!mlr || !sp || sp->path.empty()) continue;

            const std::string key = sp->path + '#' + std::to_string(mlr->meshId);
            auto& slot = mc[key];
            if (col) { slot.col = *col; slot.hasCol = true; }
            if (rb)  { slot.rb  = *rb;  slot.hasRb  = true; }
        }
        for (const auto& [key, entry] : mc)
        {
            const size_t hashPos = key.rfind('#');
            const std::string srcPath = key.substr(0, hashPos);
            const uint32_t srcMeshId = static_cast<uint32_t>(std::stoul(key.substr(hashPos + 1)));

            const auto& c = entry.col;
            const auto& r = entry.rb;
            // Fields default-initialise if a key was absent in source; the
            // loader's defaults will mirror the deserializer in ComponentSerializers.
            snprintf(buf, sizeof(buf),
                "MC src=%s srcMesh=%u hasCol=%u hasRb=%u"
                " shape=%u halfExtents=%.4f_%.4f_%.4f radius=%.4f halfHeight=%.4f"
                " meshPath=%s meshId=%u"
                " mt=%u mass=%.4f linDamp=%.4f angDamp=%.4f"
                " friction=%.4f restitution=%.4f gravity=%.4f\n",
                PercentEncode(srcPath).c_str(), srcMeshId,
                entry.hasCol ? 1u : 0u, entry.hasRb ? 1u : 0u,
                static_cast<uint32_t>(c.shape),
                c.halfExtents.x, c.halfExtents.y, c.halfExtents.z, c.radius, c.halfHeight,
                PercentEncode(c.meshLibPath).c_str(), c.meshLibMeshId,
                static_cast<uint32_t>(r.motion),
                r.mass, r.linearDamping, r.angularDamping,
                r.friction, r.restitution, r.gravityFactor);
            ss << buf;
        }
    }

    // --- Write to file ---
    const std::string text = ss.str();
    const uint32_t textLen = static_cast<uint32_t>(text.size());
    const uint32_t payloadSz = textLen + 1;

    struct WorldMeta { uint32_t nodeCount; uint32_t textLength; uint32_t _pad[2]; };
    WorldMeta meta{ static_cast<uint32_t>(nodes.size()), textLen, {0, 0} };

    AssetHeader hdr{};
    hdr.magic        = MAGIC_WORLD;
    hdr.version      = ASSET_VERSION;
    hdr.resourceType = 0;
    hdr.metadataSize = sizeof(meta);
    hdr.dataSize     = payloadSz;

    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        LOG_ERROR("WorldSerializer: cannot write '%s'", path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(text.c_str(), textLen);
    f.put('\0');

    LOG_SUCCESS("WorldSerializer: saved '%s' — %u entities, %u bytes",
                path.c_str(), static_cast<uint32_t>(nodes.size()), textLen);
    return true;
}

// ===========================================================================
// LoadWorld — generic component deserialization via ComponentSerializerRegistry
// ===========================================================================
bool Resource::LoadWorld(const std::string& path, World& world, AssetManager& assetMgr,
                          Renderer* renderer, AnimationClipSystem* animClipSys,
                          std::string* outSceneName,
                          std::string* outPostProcessConfigPath,
                          std::string* outNavMeshPath)
{
    auto& reg = GetComponentRegistry();

    // 1. Read file (pak or disk via AssetFS)
    std::vector<uint8_t> data;
    if (!::Resource::AssetFS::Get().ReadFile(path, data))
    { LOG_ERROR("WorldSerializer: cannot open '%s'", path.c_str()); return false; }

    const auto sz = data.size();
    if (sz < sizeof(AssetHeader))
    { LOG_ERROR("WorldSerializer: file too small"); return false; }

    const auto* hdr = reinterpret_cast<const AssetHeader*>(data.data());
    if (hdr->magic != MAGIC_WORLD)
    { LOG_ERROR("WorldSerializer: bad magic in '%s'", path.c_str()); return false; }

    const char* text = reinterpret_cast<const char*>(data.data() + sizeof(AssetHeader) + hdr->metadataSize);

    // 2. Parse text line by line — done BEFORE world.Clear() so we can pre-scan
    //    resource paths and kick off async loads while the old world is still
    //    rendering (SceneLoading doc v2 §8.1 Load-then-Release).
    std::string sceneName            = "Untitled";
    std::string postProcessConfigPath;
    std::string navMeshPath;
    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> compBlocks;
    int currentNodeIdx = -1;

    // Mesh-collider overrides for .iscn-spawned entities (see SaveWorld for
    // why this exists). One MC line per unique (sourcePath, sourceMeshId);
    // applied AFTER SceneInstanceLoader::Load re-spawns the scene tree.
    struct MeshColliderOverride
    {
        std::string         sourcePath;
        uint32_t            sourceMeshId = 0;
        ColliderComponent   collider;
        RigidBodyComponent  rb;
        bool                hasCollider  = false;
        bool                hasRb        = false;
    };
    std::vector<MeshColliderOverride> mcOverrides;

    std::istringstream stream(std::string(text, hdr->dataSize));
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line[0] == '#') continue;

        if (line[0] == 'W')
        {
            // W name=<scene-name> [postProcessConfig=<path>]
            std::istringstream ls(line.substr(2));
            std::string token;
            while (ls >> token)
            {
                auto eq = token.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = token.substr(0, eq);
                const std::string val = token.substr(eq + 1);
                if      (key == "name")              sceneName             = PercentDecode(val);
                else if (key == "postProcessConfig") postProcessConfigPath = PercentDecode(val);
                else if (key == "navMesh")           navMeshPath           = PercentDecode(val);
            }
        }
        else if (line[0] == 'N' && line.size() > 1 && line[1] == ' ')
        {
            NodeRecord nr;
            if (ParseNLine(line, nr))
            {
                currentNodeIdx = nr.idx;
                nodes.push_back(std::move(nr));
            }
        }
        else if (line.size() > 3 && line[0] == 'M' && line[1] == 'C' && line[2] == ' ')
        {
            // MC src=<path> srcMesh=N hasCol=1 hasRb=1 <collider fields> <rigidbody fields>
            MeshColliderOverride mco;
            std::istringstream ls(line.substr(3));
            std::string token;
            while (ls >> token)
            {
                const auto eq = token.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = token.substr(0, eq);
                const std::string v = token.substr(eq + 1);
                if      (k == "src")          mco.sourcePath = PercentDecode(v);
                else if (k == "srcMesh")      mco.sourceMeshId = static_cast<uint32_t>(std::stoul(v));
                else if (k == "hasCol")       mco.hasCollider = (std::stoi(v) != 0);
                else if (k == "hasRb")        mco.hasRb       = (std::stoi(v) != 0);
                else if (k == "shape")        mco.collider.shape = static_cast<ColliderComponent::Shape>(std::stoi(v));
                else if (k == "halfExtents")
                {
                    float x=0,y=0,z=0;
                    sscanf_s(v.c_str(), "%f_%f_%f", &x, &y, &z);
                    mco.collider.halfExtents = { x, y, z };
                }
                else if (k == "radius")       mco.collider.radius     = std::stof(v);
                else if (k == "halfHeight")   mco.collider.halfHeight = std::stof(v);
                else if (k == "meshPath")     mco.collider.meshLibPath   = PercentDecode(v);
                else if (k == "meshId")       mco.collider.meshLibMeshId = static_cast<uint32_t>(std::stoul(v));
                else if (k == "mt")           mco.rb.motion          = static_cast<RigidBodyComponent::Motion>(std::stoi(v));
                else if (k == "mass")         mco.rb.mass            = std::stof(v);
                else if (k == "linDamp")      mco.rb.linearDamping   = std::stof(v);
                else if (k == "angDamp")      mco.rb.angularDamping  = std::stof(v);
                else if (k == "friction")     mco.rb.friction        = std::stof(v);
                else if (k == "restitution")  mco.rb.restitution     = std::stof(v);
                else if (k == "gravity")      mco.rb.gravityFactor   = std::stof(v);
            }
            mco.rb.bodyId = kInvalidPhysicsBodyId;
            mcOverrides.push_back(std::move(mco));
        }
        else if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
        {
            ComponentBlock cb;
            if (ParseComponentLine(line, currentNodeIdx, cb))
                compBlocks.push_back(std::move(cb));
        }
    }

    if (outSceneName) *outSceneName = sceneName;

    // 3. Phase 1 / Phase 2 — pre-load loader-backed resources BEFORE Clear().
    //    Workers run async file I/O; main thread pumps GPU uploads. By the time
    //    Clear() drops the old world's refs, RM's path-hash cache has every
    //    shared path warm, so the deserialize loop in step 6 hits cache instead
    //    of re-reading from disk.
    //
    //    SceneRef (.iscn) paths get a separate fire-and-forget warmup pass —
    //    they have no registered IResourceLoader so we can't route them through
    //    ResourceManager, but pre-touching the bytes on a worker pulls them
    //    into the OS page cache before SceneInstanceLoader's sync read in
    //    step 8. For SceneRef-heavy worlds (Bistro, etc.) this is the biggest
    //    win; the MaterialOverride/AnimRef path covers VFX/cutscene-style worlds.
    {
        auto* rm = assetMgr.GetResourceManager();

        // 3a. Recursive page-cache warmup for .iscn — fire-and-forget; not
        //     waited on. The outer worker reads the .iscn, parses F/K/M lines,
        //     and pushes child warmups for every sibling (.meshlib, .imat,
        //     .iskel, .imorph). SceneInstanceLoader's sync reads in step 8
        //     then hit a warm kernel page cache instead of cold disk seeks.
        const std::vector<std::string> scenePaths = CollectScenePaths(compBlocks);
        for (const std::string& scenePath : scenePaths)
            EnqueueIscnRecursiveWarmup(scenePath);

        // 3b. RM-backed pre-load for loader-known extensions (textures, anims).
        if (rm)
        {
            const std::vector<PreloadEntry> preloadList = CollectPreloadPaths(compBlocks);
            std::vector<Handle> preloadHandles;
            preloadHandles.reserve(preloadList.size());
            for (const PreloadEntry& pe : preloadList)
                preloadHandles.push_back(rm->Load(pe.path, pe.type));

            if (!preloadHandles.empty() || !scenePaths.empty())
            {
                LOG_INFO("WorldSerializer: pre-loading %zu RM resources + %zu .iscn warmups",
                         preloadHandles.size(), scenePaths.size());
            }
            if (!preloadHandles.empty())
                WaitForHandlesReady(*rm, preloadHandles);
            // preloadHandles drop here. RM Handle is a POD — slots stay alive
            // because nothing explicitly Unloads them; the systems that own
            // refcounts (TextureSystem / AnimationClipSystem) will Acquire
            // them in step 6/7 and inherit the warm RM cache.
        }
    }

    // 4. Clear the old world. Component destructors release TextureSystem /
    //    MeshLibrary / AnimationClipSystem refs; underlying RM slots stay
    //    cached for path-hash hits in the next phase.
    world.Clear();

    // 5. Pre-collect skinned node indices (SceneRef components)
    struct SkinInfo { int nodeIdx = -1; std::string scenePath, animPath; };
    std::unordered_map<int, SkinInfo> skinInfoMap;
    std::unordered_set<int> skinnedIndices;

    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "SceneRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
            {
                skinInfoMap[cb.nodeIdx].nodeIdx = cb.nodeIdx;
                skinInfoMap[cb.nodeIdx].scenePath = PercentDecode(it->second);
                skinnedIndices.insert(cb.nodeIdx);
            }
        }
        else if (cb.tag == "AnimRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
            {
                skinInfoMap[cb.nodeIdx].nodeIdx = cb.nodeIdx;
                skinInfoMap[cb.nodeIdx].animPath = PercentDecode(it->second);
            }
        }
    }

    // 6. Create non-skinned entities (skinned roots are loaded from .iscn in step 8)
    std::unordered_map<int, Entity> entityMap;
    for (const auto& nr : nodes)
    {
        if (skinnedIndices.count(nr.idx)) continue;

        Entity e = world.CreateEntity();
        world.SetName(e, nr.name.empty() ? "Entity" : nr.name);
        entityMap[nr.idx] = e;

        LocalTransform lt;
        lt.translation = { nr.tx, nr.ty, nr.tz };
        lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
        lt.scale       = { nr.sx, nr.sy, nr.sz };
        world.AddComponent<LocalTransform>(e, lt);
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});
        world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
        world.AddComponent<RenderLayer>(e, RenderLayer{});
        world.AddComponent<Children>(e, Children{});

        // Parent wiring
        if (nr.parent >= 0)
        {
            auto it = entityMap.find(nr.parent);
            if (it != entityMap.end())
            {
                world.AddComponent<Parent>(e, Parent{ it->second });
                if (auto* ch = world.GetComponent<Children>(it->second))
                    ch->entities.push_back(e);
            }
        }
    }

    // 7. Apply component blocks for non-skinned entities via registry
    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "SceneRef" || cb.tag == "AnimRef") continue;
        if (skinnedIndices.count(cb.nodeIdx)) continue;

        auto it = entityMap.find(cb.nodeIdx);
        if (it == entityMap.end()) continue;

        const ComponentSerializer* ser = reg.FindByTag(cb.tag);
        if (ser)
            ser->deserialize(world, it->second, cb.kv, &assetMgr);
    }

    // Post-process: apply MaterialOverride to MaterialComponent
    for (const auto& [idx, e] : entityMap)
    {
        const auto* ovr = world.GetComponent<MaterialOverride>(e);
        auto* mat = world.GetComponent<MaterialComponent>(e);
        if (ovr && mat && !ovr->IsEmpty())
        {
            ApplyMaterialOverride(*mat, *ovr);
            mat->SetDirty();
        }
    }

    // Add default material to mesh entities that don't have one
    for (const auto& [idx, e] : entityMap)
    {
        if (world.HasComponent<MeshLibRef>(e) && !world.HasComponent<MaterialComponent>(e))
            world.AddComponent<MaterialComponent>(e, MaterialComponent{});
    }

    // 8. Skinned character reconstruction from SceneRef/AnimRef
    for (const auto& [idx, info] : skinInfoMap)
    {
        if (info.scenePath.empty()) continue;

        MeshLibrary* meshLib = renderer ? renderer->GetMeshLibrary() : nullptr;
        if (!meshLib)
        {
            LOG_ERROR("WorldSerializer: MeshLibrary not wired — cannot load skinned scene '%s'",
                      info.scenePath.c_str());
            continue;
        }
        SceneInstanceLoader::LoadResult res =
            SceneInstanceLoader::Load(info.scenePath, world, assetMgr, *meshLib, renderer);

        if (res.success && res.rootEntity != NullEntity)
        {
            entityMap[idx] = res.rootEntity;

            // Restore name and LocalTransform from N line
            for (const auto& nr : nodes)
            {
                if (nr.idx != idx) continue;
                if (!nr.name.empty())
                    world.SetName(res.rootEntity, nr.name);

                LocalTransform lt;
                lt.translation = { nr.tx, nr.ty, nr.tz };
                lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
                lt.scale       = { nr.sx, nr.sy, nr.sz };
                if (auto* existing = world.GetComponent<LocalTransform>(res.rootEntity))
                    *existing = lt;
                else
                    world.AddComponent<LocalTransform>(res.rootEntity, lt);
                break;
            }

            // Store source paths
            world.AddComponent<SceneSourcePath>(res.rootEntity, SceneSourcePath{ info.scenePath });

            // Apply component overrides on the skinned root (LightData, Billboard, etc.)
            for (const auto& cb : compBlocks)
            {
                if (cb.nodeIdx != idx) continue;
                if (cb.tag == "SceneRef" || cb.tag == "AnimRef") continue;
                const ComponentSerializer* ser = reg.FindByTag(cb.tag);
                if (ser)
                    ser->deserialize(world, res.rootEntity, cb.kv, &assetMgr);
            }

            // Bind animation if present
            if (!info.animPath.empty() && animClipSys && renderer)
            {
                auto* skelComp = world.GetComponent<SkeletonComponent>(res.rootEntity);
                if (skelComp && skelComp->assetIndex != kInvalidAnimHandle)
                {
                    AnimHandle ah = animClipSys->AcquireClip(info.animPath);
                    world.AddComponent<AnimationSourcePath>(res.rootEntity,
                        AnimationSourcePath{ info.animPath });

                    if (animClipSys->IsReady(ah))
                    {
                        const SkeletonAsset& skel = renderer->GetSkeletonRegistry().Get(skelComp->assetIndex);
                        const uint32_t clipIdx = animClipSys->BindToSkeleton(
                            ah, skel, renderer->GetClipLibrary());
                        if (clipIdx != kInvalidClipIndex)
                        {
                            auto* animComp = world.GetComponent<AnimationComponent>(res.rootEntity);
                            if (animComp) animComp->primaryClip = clipIdx;
                        }
                        const uint32_t morphIdx = animClipSys->BindMorphClip(
                            ah, renderer->GetMorphClipLibrary());
                        if (morphIdx != kInvalidClipIndex)
                        {
                            auto* morphComp = world.GetComponent<MorphComponent>(res.rootEntity);
                            if (!morphComp)
                            { MorphComponent mc{}; mc.primaryMorphClip = morphIdx; world.AddComponent(res.rootEntity, mc); }
                            else morphComp->primaryMorphClip = morphIdx;
                        }
                    }
                    else
                    {
                        PendingAnimBind pending;
                        pending.animPath = info.animPath;
                        pending.handlePacked = ah.packed;
                        world.AddComponent<PendingAnimBind>(res.rootEntity, std::move(pending));
                        LOG_INFO("WorldSerializer: queued deferred anim bind '%s' on entity %u",
                                 info.animPath.c_str(), res.rootEntity);
                    }
                }
            }

            LOG_INFO("WorldSerializer: loaded skinned entity idx=%d scene='%s' anim='%s'",
                     idx, info.scenePath.c_str(), info.animPath.c_str());
        }
        else
        {
            LOG_ERROR("WorldSerializer: failed to load .iscn '%s'", info.scenePath.c_str());
        }
    }

    // 8.5: Re-wire Parent / Children for every node based on the saved
    // parentIdx. Step 6 already wired non-skinned entities, but it could
    // not see skinned (.iscn) entities since they're only created in
    // Step 8. This second pass also reconciles the reverse case — an .iscn
    // root that was reparented under a regular entity in the editor.
    // Idempotent: skips if the current Parent already matches the saved one.
    for (const auto& nr : nodes)
    {
        if (nr.parent < 0) continue;
        auto eit = entityMap.find(nr.idx);
        if (eit == entityMap.end()) continue;
        auto pit = entityMap.find(nr.parent);
        if (pit == entityMap.end()) continue;

        const Entity child  = eit->second;
        const Entity parent = pit->second;

        Parent* p = world.GetComponent<Parent>(child);
        if (p && p->entity == parent) continue;
        if (p) p->entity = parent;
        else   world.AddComponent<Parent>(child, Parent{ parent });

        Children* ch = world.GetComponent<Children>(parent);
        if (!ch)
        {
            Children newCh; newCh.entities.push_back(child);
            world.AddComponent<Children>(parent, std::move(newCh));
        }
        else
        {
            auto& v = ch->entities;
            if (std::find(v.begin(), v.end(), child) == v.end())
                v.push_back(child);
        }
    }

    LOG_SUCCESS("WorldSerializer: loaded '%s' — %zu entities, scene='%s'",
                path.c_str(), nodes.size(), sceneName.c_str());

    // 9. Apply referenced post-processing config (if any, and a Renderer is
    //    available). Missing file is non-fatal — worst case the pass keeps
    //    whatever state it was in before the load.
    if (outPostProcessConfigPath) *outPostProcessConfigPath = postProcessConfigPath;
    if (outNavMeshPath)            *outNavMeshPath           = navMeshPath;
    if (!postProcessConfigPath.empty() && renderer)
    {
        PostProcessConfig cfg;
        if (LoadPostProcessConfig(postProcessConfigPath, cfg))
        {
            cfg.ApplyTo(*renderer);
            LOG_INFO("WorldSerializer: applied post-process config '%s'",
                     postProcessConfigPath.c_str());
        }
    }

    // Apply Mesh-collider overrides to every .iscn-spawned mesh entity
    // whose (sourcePath, sourceMeshId) matches an MC line. Done last so the
    // SceneInstanceLoader path has fully populated the world. PhysicsSystem's
    // auto-prewarm picks up the new Mesh ColliderComponents on its next
    // Update tick — no manual Prewarm call needed here.
    if (!mcOverrides.empty())
    {
        uint32_t applied = 0;
        for (const auto& mco : mcOverrides)
        {
            world.ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
            {
                if (ref.meshId != mco.sourceMeshId) return;
                const MeshSourcePath* sp = world.GetComponent<MeshSourcePath>(e);
                if (!sp || sp->path != mco.sourcePath) return;

                if (mco.hasCollider)
                {
                    if (world.HasComponent<ColliderComponent>(e))
                        *world.GetComponent<ColliderComponent>(e) = mco.collider;
                    else
                        world.AddComponent<ColliderComponent>(e, mco.collider);
                }
                if (mco.hasRb)
                {
                    if (world.HasComponent<RigidBodyComponent>(e))
                    {
                        auto* rb = world.GetComponent<RigidBodyComponent>(e);
                        *rb = mco.rb;
                        rb->bodyId = kInvalidPhysicsBodyId; // force re-create on next physics tick
                    }
                    else
                    {
                        world.AddComponent<RigidBodyComponent>(e, mco.rb);
                    }
                }
                ++applied;
            });
        }
        LOG_INFO("WorldSerializer: applied %u Mesh collider overrides from %zu MC lines",
                 applied, mcOverrides.size());
    }

    return true;
}
