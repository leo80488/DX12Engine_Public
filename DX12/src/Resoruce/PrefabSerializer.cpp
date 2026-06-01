#include "Resource/PrefabSerializer.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/ComponentSerializers.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/FollowComponents.h"
#include "Scene/SceneInstanceLoader.h"
#include "Graphics/Renderer.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace DirectX;

namespace
{

// ---------------------------------------------------------------------------
// Percent-encode / decode (space <-> %20, percent <-> %25)
// ---------------------------------------------------------------------------
std::string PercentEncode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s)
    {
        if      (c == '%')  r += "%25";
        else if (c == ' ')  r += "%20";
        else                r += static_cast<char>(c);
    }
    return r;
}

std::string PercentDecode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            try { r += static_cast<char>(std::stoi(hex, nullptr, 16)); }
            catch (...) { r += s[i]; }
            i += 3;
        }
        else r += s[i++];
    }
    return r;
}

// ---------------------------------------------------------------------------
// Collect the entity subtree into a flat list (DFS order, parents before children).
// ---------------------------------------------------------------------------
struct NodeInfo
{
    int    idx        { -1 };
    int    parentIdx  { -1 };
    Entity entity     { NullEntity };
};

void CollectNodes(World& world, Entity e, int parentIdx,
                  std::unordered_map<Entity, int>& entityToIdx,
                  std::vector<NodeInfo>& out)
{
    if (!world.IsAlive(e)) return;
    if (entityToIdx.count(e)) return;  // already collected (avoids cycles via shared subtree)
    const int myIdx = static_cast<int>(out.size());
    entityToIdx[e] = myIdx;
    out.push_back({ myIdx, parentIdx, e });
    const Children* ch = world.GetComponent<Children>(e);
    if (ch)
        for (Entity child : ch->entities)
            CollectNodes(world, child, myIdx, entityToIdx, out);
}

// ---------------------------------------------------------------------------
// CollectLinkedFollowers — extend the prefab set with any entity outside the
// current set that holds a cross-entity reference (FollowSocket, FollowEntity,
// SkeletonRef) into something already in the set. Iterates until fixed point
// because newly-added entities can themselves bring in further dependents.
// New roots are added with parent=-1 (siblings of the original root).
// ---------------------------------------------------------------------------
void CollectLinkedFollowers(World& world,
                            std::unordered_map<Entity, int>& entityToIdx,
                            std::vector<NodeInfo>& nodes)
{
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (Entity e : world.GetEntities())
        {
            if (!world.IsAlive(e)) continue;
            if (entityToIdx.count(e)) continue;

            bool linksIn = false;
            if (auto* fs = world.GetComponent<FollowSocketComponent>(e);
                fs && entityToIdx.count(fs->target.entity)) linksIn = true;
            if (!linksIn)
                if (auto* fe = world.GetComponent<FollowEntityComponent>(e);
                    fe && entityToIdx.count(fe->target.entity)) linksIn = true;
            if (!linksIn)
                if (auto* sr = world.GetComponent<SkeletonRef>(e);
                    sr && entityToIdx.count(sr->entity)) linksIn = true;

            if (linksIn)
            {
                CollectNodes(world, e, /*parentIdx=*/-1, entityToIdx, nodes);
                changed = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Cross-entity ref components are stored under our own tags ("PrefabFollowSocket"
// etc.) so the registry walk in LoadPrefab silently skips them and our manual
// fixup pass picks them up after every node has a fresh entity ID. The matrix
// is flattened to 16 underscore-separated floats to keep the line short.
// ---------------------------------------------------------------------------
std::string MatrixToString(const DirectX::XMFLOAT4X4& m)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
        "%.6f_%.6f_%.6f_%.6f_%.6f_%.6f_%.6f_%.6f_"
        "%.6f_%.6f_%.6f_%.6f_%.6f_%.6f_%.6f_%.6f",
        m._11, m._12, m._13, m._14,
        m._21, m._22, m._23, m._24,
        m._31, m._32, m._33, m._34,
        m._41, m._42, m._43, m._44);
    return buf;
}

DirectX::XMFLOAT4X4 ParseMatrix(const std::string& s)
{
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
    float* p = reinterpret_cast<float*>(&m);
    size_t start = 0;
    for (int i = 0; i < 16; ++i)
    {
        size_t end = s.find('_', start);
        if (end == std::string::npos) end = s.size();
        try { p[i] = std::stof(s.substr(start, end - start)); } catch (...) {}
        if (end == s.size()) break;
        start = end + 1;
    }
    return m;
}

// Decompose a 4x4 to XYZ Euler degrees so an inspector can show stable
// rotation values without a per-frame quat↔euler round-trip. Lossy near
// gimbal lock — acceptable for offset matrices.
DirectX::XMFLOAT3 DecomposeToEulerDeg(const DirectX::XMFLOAT4X4& m)
{
    using namespace DirectX;
    XMVECTOR vScl, vRot, vTrn;
    if (!XMMatrixDecompose(&vScl, &vRot, &vTrn, XMLoadFloat4x4(&m)))
        return { 0.f, 0.f, 0.f };
    XMFLOAT4 q; XMStoreFloat4(&q, vRot);
    const float ysqr = q.y * q.y;
    const float t0 = 2.0f * (q.w * q.x + q.y * q.z);
    const float t1 = 1.0f - 2.0f * (q.x * q.x + ysqr);
    const float rx = std::atan2(t0, t1);
    float t2 = 2.0f * (q.w * q.y - q.z * q.x);
    t2 = (t2 >  1.0f) ?  1.0f : (t2 < -1.0f ? -1.0f : t2);
    const float ry = std::asin(t2);
    const float t3 = 2.0f * (q.w * q.z + q.x * q.y);
    const float t4 = 1.0f - 2.0f * (ysqr + q.z * q.z);
    const float rz = std::atan2(t3, t4);
    return { XMConvertToDegrees(rx), XMConvertToDegrees(ry), XMConvertToDegrees(rz) };
}

// ---------------------------------------------------------------------------
// Apply MaterialOverride property bag onto a MaterialComponent.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Parse N line → node record (transform + name).
// ---------------------------------------------------------------------------
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
            else if (key == "name")   out.name   = PercentDecode(val);
        }
        catch (...) {}
    }
    return out.idx >= 0;
}

// ---------------------------------------------------------------------------
// Parse indented component line → tag + KVMap.
// Format: "  Tag: key=val key=val ..."
// ---------------------------------------------------------------------------
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

} // anonymous namespace

// ===========================================================================
// SavePrefab — generic component serialization via ComponentSerializerRegistry
// ===========================================================================
bool Resource::SavePrefab(Entity root, World& world, const std::string& path)
{
    auto& reg = GetComponentRegistry();

    std::unordered_map<Entity, int> entityToIdx;
    std::vector<NodeInfo> nodes;
    CollectNodes(world, root, -1, entityToIdx, nodes);

    if (nodes.empty())
    {
        LOG_ERROR("PrefabSerializer: entity %u is not alive", root);
        return false;
    }

    // Auto-include entities that reference anything in the saved subtree —
    // e.g. trails Following a model bone via FollowSocket. They become extra
    // top-level nodes (parent=-1) and their cross-refs get rewritten to
    // prefab-local indices below.
    const size_t coreCount = nodes.size();
    CollectLinkedFollowers(world, entityToIdx, nodes);
    if (nodes.size() > coreCount)
        LOG_INFO("PrefabSerializer: auto-included %zu linked entities (followers)",
                 nodes.size() - coreCount);

    std::ostringstream ss;
    ss << "# DX12 Engine Prefab\n";
    char buf[512];

    for (const NodeInfo& ni : nodes)
    {
        const Entity e = ni.entity;

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
            "N idx=%d parent=%d"
            " tx=%.6f ty=%.6f tz=%.6f"
            " qx=%.6f qy=%.6f qz=%.6f qw=%.6f"
            " sx=%.6f sy=%.6f sz=%.6f",
            ni.idx, ni.parentIdx,
            tx, ty, tz, qx, qy, qz, qw, sx, sy, sz);
        ss << buf << " name=" << PercentEncode(world.GetName(e)) << "\n";

        // --- Pre-step: auto-create .imat if entity has MaterialComponent but no MaterialSourcePath ---
        if (world.HasComponent<MaterialComponent>(e) && !world.HasComponent<MaterialSourcePath>(e))
        {
            const MaterialComponent* mat = world.GetComponent<MaterialComponent>(e);
            namespace fs = std::filesystem;
            const fs::path prefabDir = fs::path(path).parent_path();
            const fs::path matDir    = prefabDir / "Materials";
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
            world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ matAbsPath });
        }

        // --- Generic component serialization via registry ---
        for (const auto& [typeIdx, serializer] : reg.All())
        {
            if (serializer.has(world, e))
                serializer.serialize(world, e, ss);
        }

        // --- Cross-entity references --------------------------------------
        // Tag prefix "Prefab*" so the registry walk in LoadPrefab ignores
        // them; our deferred fixup picks them up after every new entity is
        // created.
        //
        // Target-resolution policy: if target is in the prefab subtree, write
        // its prefab-local idx so load wires it up automatically. If it's
        // OUTSIDE (e.g. a weapon prefab whose owner lives in the scene),
        // write target=-1 — the load path still creates the component (with
        // NullEntityHandle) so authored offset/socketIndex survive, and
        // equipment code rebinds the target at runtime. Prior behaviour
        // silently dropped the whole reference, which made standalone weapon
        // prefabs lose their attachment data.
        if (auto* fs = world.GetComponent<FollowSocketComponent>(e))
        {
            auto it = entityToIdx.find(fs->target.entity);
            const int tgtIdx = (it != entityToIdx.end()) ? it->second : -1;
            ss << "  PrefabFollowSocket: target=" << tgtIdx
               << " socket=" << fs->socketIndex
               << " m=" << MatrixToString(fs->localOffset) << "\n";
        }
        if (auto* fe = world.GetComponent<FollowEntityComponent>(e))
        {
            auto it = entityToIdx.find(fe->target.entity);
            const int tgtIdx = (it != entityToIdx.end()) ? it->second : -1;
            ss << "  PrefabFollowEntity: target=" << tgtIdx
               << " m=" << MatrixToString(fe->localOffset) << "\n";
        }
        if (auto* sr = world.GetComponent<SkeletonRef>(e))
        {
            // SkeletonRef is a hard skinning dependency — without a target
            // the follower can't be rendered. Keep the in-set gate.
            if (auto it = entityToIdx.find(sr->entity); it != entityToIdx.end())
                ss << "  PrefabSkeletonRef: target=" << it->second << "\n";
        }
    }

    // --- Write binary blob ---
    const std::string text       = ss.str();
    const uint32_t    textLength = static_cast<uint32_t>(text.size());
    const uint32_t    payloadSz  = textLength + 1;

    PrefabMetadata meta{};
    meta.nodeCount  = static_cast<uint32_t>(nodes.size());
    meta.textLength = textLength;

    AssetHeader hdr{};
    hdr.magic        = MAGIC_PREFAB;
    hdr.version      = ASSET_VERSION;
    hdr.resourceType = static_cast<uint16_t>(ResourceType::Unknown);
    hdr.metadataSize = sizeof(PrefabMetadata);
    hdr.dataSize     = payloadSz;

    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        LOG_ERROR("PrefabSerializer: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&hdr),  sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(text.c_str(), textLength);
    const char nul = '\0';
    f.write(&nul, 1);

    if (!f.good())
    {
        LOG_ERROR("PrefabSerializer: write error for '%s'", path.c_str());
        return false;
    }

    LOG_INFO("PrefabSerializer: saved '%s' — %u nodes, %u bytes",
             path.c_str(), static_cast<uint32_t>(nodes.size()), textLength);
    return true;
}

// ===========================================================================
// LoadPrefab — generic component deserialization via ComponentSerializerRegistry
// ===========================================================================
Entity Resource::LoadPrefab(const std::string& path, World& world, AssetManager& assetMgr,
                            Renderer* renderer, AnimationClipSystem* animClipSys)
{
    auto& reg = GetComponentRegistry();

    // 1. Read and validate blob
    std::vector<uint8_t> data;
    if (!::Resource::AssetFS::Get().ReadFile(path, data))
    {
        LOG_ERROR("PrefabSerializer: cannot open '%s'", path.c_str());
        return NullEntity;
    }

    if (!ValidateHeader(data.data(), data.size(), MAGIC_PREFAB))
    {
        LOG_ERROR("PrefabSerializer: invalid .ipfb header in '%s'", path.c_str());
        return NullEntity;
    }

    const char* text = reinterpret_cast<const char*>(GetPayload(data.data()));
    const size_t textLen = GetHeader(data.data())->dataSize;

    // 2. Parse N lines and indented component blocks
    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> compBlocks;
    int currentNodeIdx = -1;

    std::istringstream stream(std::string(text, textLen));
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line[0] == '#') continue;

        if (line[0] == 'N' && line.size() > 1 && line[1] == ' ')
        {
            NodeRecord nr;
            if (ParseNLine(line, nr))
            {
                currentNodeIdx = nr.idx;
                nodes.push_back(std::move(nr));
            }
        }
        else if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
        {
            ComponentBlock cb;
            if (ParseComponentLine(line, currentNodeIdx, cb))
                compBlocks.push_back(std::move(cb));
        }
    }

    if (nodes.empty())
    {
        LOG_ERROR("PrefabSerializer: no nodes in '%s'", path.c_str());
        return NullEntity;
    }

    // 3. Detect skinned prefab (SceneRef component). sceneIdx tracks which
    // N-line owns the SceneRef so the prefab root can be a wrapper entity
    // (e.g. user wrapped the character under an empty entity); without this,
    // the legacy code assumed sceneIdx == 0 and mis-mapped every index when
    // the wrapper sat at idx 0 and the .iscn root at idx 1.
    //
    // Multi-SceneRef: a prefab may carry several SceneRef nodes when
    // CollectLinkedFollowers auto-included a follower whose subtree owns its
    // own .iscn (e.g. a character + an equipped weapon prefab). The first
    // (lowest-idx) entry drives animation binding + prefab root; the rest are
    // loaded into the same entityMap so their idx ranges stay consistent.
    struct SceneRefEntry { int idx; std::string path; };
    std::vector<SceneRefEntry> sceneRefs;
    std::string animPath;
    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "SceneRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
                sceneRefs.push_back({ cb.nodeIdx, PercentDecode(it->second) });
        }
        else if (cb.tag == "AnimRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end()) animPath = PercentDecode(it->second);
        }
    }
    std::sort(sceneRefs.begin(), sceneRefs.end(),
              [](const SceneRefEntry& a, const SceneRefEntry& b) { return a.idx < b.idx; });
    const std::string scenePath = sceneRefs.empty() ? std::string{} : sceneRefs.front().path;
    const int         sceneIdx  = sceneRefs.empty() ? -1            : sceneRefs.front().idx;

    // ---- Skinned character flow ----
    if (!scenePath.empty() && renderer)
    {
        MeshLibrary* meshLib = renderer->GetMeshLibrary();
        if (!meshLib)
        {
            LOG_ERROR("PrefabSerializer: MeshLibrary not wired on renderer — cannot load scene '%s'",
                      scenePath.c_str());
            return NullEntity;
        }
        auto res = SceneInstanceLoader::Load(scenePath, world, assetMgr, *meshLib, renderer);
        if (!res.success || res.rootEntity == NullEntity)
        {
            LOG_ERROR("PrefabSerializer: failed to load .iscn '%s'", scenePath.c_str());
            return NullEntity;
        }

        // Apply transform/name from the N-line that owned the SceneRef block.
        // Defaults to nodes[0] for legacy prefabs (saved before wrapping was
        // supported, where the prefab root IS the .iscn root).
        const int sceneIdxLocal = (sceneIdx >= 0) ? sceneIdx : 0;
        for (const auto& nr : nodes)
        {
            if (nr.idx != sceneIdxLocal) continue;
            LocalTransform lt;
            lt.translation = { nr.tx, nr.ty, nr.tz };
            lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
            lt.scale       = { nr.sx, nr.sy, nr.sz };
            if (auto* existing = world.GetComponent<LocalTransform>(res.rootEntity))
                *existing = lt;
            if (!nr.name.empty())
                world.SetName(res.rootEntity, nr.name);
            break;
        }

        // Collect loaded entities in DFS order for index-based component application
        std::vector<Entity> loadedEntities;
        std::function<void(Entity)> collectDFS = [&](Entity ent) {
            loadedEntities.push_back(ent);
            const Children* ch = world.GetComponent<Children>(ent);
            if (ch) for (Entity child : ch->entities) collectDFS(child);
        };
        collectDFS(res.rootEntity);

        // Build idx → entity map. The .iscn DFS fills [sceneIdxLocal .. sceneIdxLocal+N-1];
        // every N-line outside that range is an "extra" entity (wrapper
        // empty, trail attached to a bone via FollowSocket, etc.) and gets
        // created from scratch in the loop below.
        std::unordered_map<int, Entity> entityMap;
        for (size_t i = 0; i < loadedEntities.size(); ++i)
            entityMap[sceneIdxLocal + static_cast<int>(i)] = loadedEntities[i];

        // Multi-SceneRef secondary load: spawn each remaining SceneRef'd
        // subtree and merge its DFS entities into entityMap. Without this,
        // a prefab containing e.g. "character + equipped weapon" only
        // materialised the LAST SceneRef and the other became 30 broken
        // wrapper entities — Sockets pointing at bone 42 on an entity with
        // no SkeletonComponent, FollowSocket reading an identity socket
        // transform, and an Inf WorldAabb that AV'd SceneBVH on the first
        // BuildRenderScene tick.
        for (size_t s = 1; s < sceneRefs.size(); ++s)
        {
            const SceneRefEntry& srf = sceneRefs[s];
            auto resN = SceneInstanceLoader::Load(srf.path, world, assetMgr, *meshLib, renderer);
            if (!resN.success || resN.rootEntity == NullEntity)
            {
                LOG_WARNING("PrefabSerializer: secondary scene '%s' failed to load — idx=%d skipped",
                            srf.path.c_str(), srf.idx);
                continue;
            }

            // N-line override (transform + name) — same convention as primary.
            for (const auto& nr : nodes)
            {
                if (nr.idx != srf.idx) continue;
                LocalTransform lt;
                lt.translation = { nr.tx, nr.ty, nr.tz };
                lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
                lt.scale       = { nr.sx, nr.sy, nr.sz };
                if (auto* existing = world.GetComponent<LocalTransform>(resN.rootEntity))
                    *existing = lt;
                if (!nr.name.empty())
                    world.SetName(resN.rootEntity, nr.name);
                break;
            }

            std::vector<Entity> nLoaded;
            std::function<void(Entity)> collectDFSn = [&](Entity ent) {
                nLoaded.push_back(ent);
                const Children* ch = world.GetComponent<Children>(ent);
                if (ch) for (Entity child : ch->entities) collectDFSn(child);
            };
            collectDFSn(resN.rootEntity);

            for (size_t i = 0; i < nLoaded.size(); ++i)
                entityMap[srf.idx + static_cast<int>(i)] = nLoaded[i];

            // Append to loadedEntities so the post-process MaterialOverride
            // loop reaches secondary-scene entities too.
            loadedEntities.insert(loadedEntities.end(), nLoaded.begin(), nLoaded.end());

            if (!world.HasComponent<SceneSourcePath>(resN.rootEntity))
                world.AddComponent<SceneSourcePath>(resN.rootEntity, SceneSourcePath{ srf.path });
        }

        std::vector<NodeRecord> sortedNodes = nodes;
        std::sort(sortedNodes.begin(), sortedNodes.end(),
                  [](const NodeRecord& a, const NodeRecord& b) { return a.idx < b.idx; });

        for (const NodeRecord& nr : sortedNodes)
        {
            if (entityMap.count(nr.idx)) continue;       // already in (loaded scene)

            const Entity e = world.CreateEntity();
            world.SetName(e, nr.name.empty() ? "prefab_extra" : nr.name);

            LocalTransform lt;
            lt.translation = { nr.tx, nr.ty, nr.tz };
            lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
            lt.scale       = { nr.sx, nr.sy, nr.sz };
            world.AddComponent<LocalTransform> (e, lt);
            world.AddComponent<GlobalTransform>(e, GlobalTransform{});
            world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
            world.AddComponent<RenderLayer>    (e, RenderLayer{});
            world.AddComponent<Children>       (e, Children{});

            if (nr.parent >= 0)
            {
                auto pit = entityMap.find(nr.parent);
                if (pit != entityMap.end())
                {
                    world.AddComponent<Parent>(e, Parent{ pit->second });
                    if (Children* pc = world.GetComponent<Children>(pit->second))
                        pc->entities.push_back(e);
                }
            }
            entityMap[nr.idx] = e;
        }

        // Apply component blocks via registry — covers BOTH loaded-scene
        // entities and freshly-created extras.
        for (const auto& cb : compBlocks)
        {
            if (cb.tag == "SceneRef" || cb.tag == "AnimRef") continue;
            auto it = entityMap.find(cb.nodeIdx);
            if (it == entityMap.end()) continue;

            const ComponentSerializer* ser = reg.FindByTag(cb.tag);
            if (ser)
                ser->deserialize(world, it->second, cb.kv, &assetMgr);
        }

        // ---- Fix up cross-entity references (trails → bones, etc.) -------
        auto resolveTargetIdx = [&](const KVMap& kv) -> Entity
        {
            auto it = kv.find("target");
            if (it == kv.end()) return NullEntity;
            try
            {
                const int targetIdx = std::stoi(it->second);
                auto m = entityMap.find(targetIdx);
                return (m != entityMap.end()) ? m->second : NullEntity;
            }
            catch (...) { return NullEntity; }
        };

        for (const auto& cb : compBlocks)
        {
            auto entIt = entityMap.find(cb.nodeIdx);
            if (entIt == entityMap.end()) continue;
            const Entity e = entIt->second;

            if (cb.tag == "PrefabFollowSocket")
            {
                // target=-1 (saved with target outside prefab) → resolveTargetIdx
                // returns NullEntity. Component is still created so authored
                // offset/socketIndex persist; runtime equip rebinds the target.
                const Entity tgt = resolveTargetIdx(cb.kv);
                FollowSocketComponent fs;
                fs.target = (tgt != NullEntity) ? world.MakeHandle(tgt)
                                                : NullEntityHandle;
                if (auto it = cb.kv.find("socket"); it != cb.kv.end())
                    try { fs.socketIndex = static_cast<uint32_t>(std::stoul(it->second)); } catch (...) {}
                if (auto it = cb.kv.find("m"); it != cb.kv.end())
                {
                    fs.localOffset      = ParseMatrix(it->second);
                    fs.rotationEulerDeg = DecomposeToEulerDeg(fs.localOffset);
                }
                world.AddComponent<FollowSocketComponent>(e, fs);
            }
            else if (cb.tag == "PrefabFollowEntity")
            {
                const Entity tgt = resolveTargetIdx(cb.kv);
                FollowEntityComponent fe;
                fe.target = (tgt != NullEntity) ? world.MakeHandle(tgt)
                                                : NullEntityHandle;
                if (auto it = cb.kv.find("m"); it != cb.kv.end())
                {
                    fe.localOffset      = ParseMatrix(it->second);
                    fe.rotationEulerDeg = DecomposeToEulerDeg(fe.localOffset);
                }
                world.AddComponent<FollowEntityComponent>(e, fe);
            }
            else if (cb.tag == "PrefabSkeletonRef")
            {
                const Entity tgt = resolveTargetIdx(cb.kv);
                if (tgt == NullEntity) continue;  // hard dep — skip if unresolved
                world.AddComponent<SkeletonRef>(e, SkeletonRef{ tgt });
            }
        }

        // Post-process: apply MaterialOverride to MaterialComponent
        for (Entity ent : loadedEntities)
        {
            const auto* ovr = world.GetComponent<MaterialOverride>(ent);
            auto* mat = world.GetComponent<MaterialComponent>(ent);
            if (ovr && mat && !ovr->IsEmpty())
            {
                ApplyMaterialOverride(*mat, *ovr);
                mat->SetDirty();
            }
        }

        // Store source paths on root
        if (!world.HasComponent<SceneSourcePath>(res.rootEntity))
            world.AddComponent<SceneSourcePath>(res.rootEntity, SceneSourcePath{ scenePath });

        // Bind animation
        if (!animPath.empty() && animClipSys)
        {
            auto* skelComp = world.GetComponent<SkeletonComponent>(res.rootEntity);
            if (skelComp && skelComp->assetIndex != kInvalidAnimHandle)
            {
                auto ah = animClipSys->AcquireClip(animPath);
                if (!world.HasComponent<AnimationSourcePath>(res.rootEntity))
                    world.AddComponent<AnimationSourcePath>(res.rootEntity,
                        AnimationSourcePath{ animPath });

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
                    pending.animPath    = animPath;
                    pending.handlePacked = ah.packed;
                    world.AddComponent<PendingAnimBind>(res.rootEntity, std::move(pending));
                    LOG_INFO("PrefabSerializer: queued deferred anim bind '%s' on entity %u",
                             animPath.c_str(), res.rootEntity);
                }
            }
        }

        // Final pass: re-wire Parent/Children from saved parentIdx for every
        // node. This handles two cases SceneInstanceLoader can't:
        //   1. The .iscn root has a wrapper parent (sceneIdx > 0).
        //   2. Extras created with a parent pointing into the .iscn DFS
        //      (e.g. a trail node parented under a specific bone).
        // Idempotent for the in-.iscn relationships SceneInstanceLoader
        // already wired.
        for (const NodeRecord& nr : nodes)
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

        // Prefab root = entity whose saved parentIdx is -1 (top of the prefab).
        // For legacy prefabs this is the .iscn root. For wrapped prefabs it's
        // the empty/wrapper entity that holds the .iscn root as its child.
        Entity prefabRoot = res.rootEntity;
        for (const NodeRecord& nr : nodes)
        {
            if (nr.parent != -1) continue;
            auto it = entityMap.find(nr.idx);
            if (it != entityMap.end()) { prefabRoot = it->second; break; }
        }

        LOG_SUCCESS("PrefabSerializer: loaded skinned prefab '%s' — root=%u sceneIdx=%d",
                    path.c_str(), prefabRoot, sceneIdxLocal);
        return prefabRoot;
    }

    // ---- Standard (non-skinned) prefab ----
    std::sort(nodes.begin(), nodes.end(),
              [](const NodeRecord& a, const NodeRecord& b) { return a.idx < b.idx; });

    std::unordered_map<int, Entity> entityMap;
    entityMap.reserve(nodes.size());
    Entity rootEntity = NullEntity;

    for (const NodeRecord& nr : nodes)
    {
        const Entity e = world.CreateEntity();
        world.SetName(e, nr.name.empty() ? "prefab_node" : nr.name);
        entityMap[nr.idx] = e;
        if (rootEntity == NullEntity) rootEntity = e;

        LocalTransform lt;
        lt.translation = { nr.tx, nr.ty, nr.tz };
        lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
        lt.scale       = { nr.sx, nr.sy, nr.sz };

        world.AddComponent<LocalTransform> (e, lt);
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});
        world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
        world.AddComponent<RenderLayer>    (e, RenderLayer{});
        world.AddComponent<Children>       (e, Children{});

        if (nr.parent >= 0)
        {
            auto it = entityMap.find(nr.parent);
            if (it != entityMap.end())
            {
                world.AddComponent<Parent>(e, Parent{ it->second });
                if (Children* pc = world.GetComponent<Children>(it->second))
                    pc->entities.push_back(e);
            }
        }
    }

    // Apply all component blocks via registry
    for (const auto& cb : compBlocks)
    {
        auto it = entityMap.find(cb.nodeIdx);
        if (it == entityMap.end()) continue;

        const ComponentSerializer* ser = reg.FindByTag(cb.tag);
        if (ser)
            ser->deserialize(world, it->second, cb.kv, &assetMgr);
    }

    // ---- Pass 2: fix up cross-entity references --------------------------
    // PrefabFollowSocket / PrefabFollowEntity / PrefabSkeletonRef hold
    // prefab-local indices in their `target=N` field. Resolve those against
    // the freshly built entityMap.
    auto resolveTargetIdx = [&](const KVMap& kv) -> Entity
    {
        auto it = kv.find("target");
        if (it == kv.end()) return NullEntity;
        try
        {
            const int targetIdx = std::stoi(it->second);
            auto m = entityMap.find(targetIdx);
            return (m != entityMap.end()) ? m->second : NullEntity;
        }
        catch (...) { return NullEntity; }
    };

    for (const auto& cb : compBlocks)
    {
        auto entIt = entityMap.find(cb.nodeIdx);
        if (entIt == entityMap.end()) continue;
        const Entity e = entIt->second;

        if (cb.tag == "PrefabFollowSocket")
        {
            // target=-1 → component created with NullEntityHandle; equip code rebinds.
            const Entity tgt = resolveTargetIdx(cb.kv);
            FollowSocketComponent fs;
            fs.target = (tgt != NullEntity) ? world.MakeHandle(tgt)
                                            : NullEntityHandle;
            if (auto it = cb.kv.find("socket"); it != cb.kv.end())
                try { fs.socketIndex = static_cast<uint32_t>(std::stoul(it->second)); } catch (...) {}
            if (auto it = cb.kv.find("m"); it != cb.kv.end())
            {
                fs.localOffset      = ParseMatrix(it->second);
                fs.rotationEulerDeg = DecomposeToEulerDeg(fs.localOffset);
            }
            world.AddComponent<FollowSocketComponent>(e, fs);
        }
        else if (cb.tag == "PrefabFollowEntity")
        {
            const Entity tgt = resolveTargetIdx(cb.kv);
            FollowEntityComponent fe;
            fe.target = (tgt != NullEntity) ? world.MakeHandle(tgt)
                                            : NullEntityHandle;
            if (auto it = cb.kv.find("m"); it != cb.kv.end())
            {
                fe.localOffset      = ParseMatrix(it->second);
                fe.rotationEulerDeg = DecomposeToEulerDeg(fe.localOffset);
            }
            world.AddComponent<FollowEntityComponent>(e, fe);
        }
        else if (cb.tag == "PrefabSkeletonRef")
        {
            const Entity tgt = resolveTargetIdx(cb.kv);
            if (tgt == NullEntity) continue;  // hard dep — skip if unresolved
            world.AddComponent<SkeletonRef>(e, SkeletonRef{ tgt });
        }
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

    LOG_SUCCESS("PrefabSerializer: loaded '%s' — %u nodes, root=%u",
                path.c_str(), static_cast<uint32_t>(nodes.size()), rootEntity);
    return rootEntity;
}
