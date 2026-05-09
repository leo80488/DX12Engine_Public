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

        // --- Cross-entity references: write only if target is also being
        // saved. Tag prefix "Prefab*" so the registry walk in LoadPrefab
        // ignores them (no FindByTag match) and our deferred fixup picks
        // them up after every new entity is created. ---
        if (auto* fs = world.GetComponent<FollowSocketComponent>(e))
        {
            if (auto it = entityToIdx.find(fs->target.entity); it != entityToIdx.end())
                ss << "  PrefabFollowSocket: target=" << it->second
                   << " socket=" << fs->socketIndex
                   << " m=" << MatrixToString(fs->localOffset) << "\n";
        }
        if (auto* fe = world.GetComponent<FollowEntityComponent>(e))
        {
            if (auto it = entityToIdx.find(fe->target.entity); it != entityToIdx.end())
                ss << "  PrefabFollowEntity: target=" << it->second
                   << " m=" << MatrixToString(fe->localOffset) << "\n";
        }
        if (auto* sr = world.GetComponent<SkeletonRef>(e))
        {
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

    // 3. Detect skinned prefab (SceneRef component)
    std::string scenePath, animPath;
    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "SceneRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end()) scenePath = PercentDecode(it->second);
        }
        else if (cb.tag == "AnimRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end()) animPath = PercentDecode(it->second);
        }
    }

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

        // Apply transform from first N line
        if (!nodes.empty())
        {
            const auto& nr = nodes[0];
            LocalTransform lt;
            lt.translation = { nr.tx, nr.ty, nr.tz };
            lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
            lt.scale       = { nr.sx, nr.sy, nr.sz };
            if (auto* existing = world.GetComponent<LocalTransform>(res.rootEntity))
                *existing = lt;
            if (!nr.name.empty())
                world.SetName(res.rootEntity, nr.name);
        }

        // Collect loaded entities in DFS order for index-based component application
        std::vector<Entity> loadedEntities;
        std::function<void(Entity)> collectDFS = [&](Entity ent) {
            loadedEntities.push_back(ent);
            const Children* ch = world.GetComponent<Children>(ent);
            if (ch) for (Entity child : ch->entities) collectDFS(child);
        };
        collectDFS(res.rootEntity);

        // Build idx → entity map. The loaded scene tree fills [0..N-1]; any
        // N-line beyond that is an "extra" entity (e.g. trail attached to a
        // bone via FollowSocket) that wasn't part of the .iscn — we create
        // those fresh below.
        std::unordered_map<int, Entity> entityMap;
        for (size_t i = 0; i < loadedEntities.size(); ++i)
            entityMap[static_cast<int>(i)] = loadedEntities[i];

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
            world.AddComponent<Visibility>     (e, Visibility{});
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
                const Entity tgt = resolveTargetIdx(cb.kv);
                if (tgt == NullEntity) continue;
                FollowSocketComponent fs;
                fs.target = world.MakeHandle(tgt);
                if (auto it = cb.kv.find("socket"); it != cb.kv.end())
                    try { fs.socketIndex = static_cast<uint32_t>(std::stoul(it->second)); } catch (...) {}
                if (auto it = cb.kv.find("m"); it != cb.kv.end())
                    fs.localOffset = ParseMatrix(it->second);
                world.AddComponent<FollowSocketComponent>(e, fs);
            }
            else if (cb.tag == "PrefabFollowEntity")
            {
                const Entity tgt = resolveTargetIdx(cb.kv);
                if (tgt == NullEntity) continue;
                FollowEntityComponent fe;
                fe.target = world.MakeHandle(tgt);
                if (auto it = cb.kv.find("m"); it != cb.kv.end())
                    fe.localOffset = ParseMatrix(it->second);
                world.AddComponent<FollowEntityComponent>(e, fe);
            }
            else if (cb.tag == "PrefabSkeletonRef")
            {
                const Entity tgt = resolveTargetIdx(cb.kv);
                if (tgt == NullEntity) continue;
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

        LOG_SUCCESS("PrefabSerializer: loaded skinned prefab '%s' — root=%u",
                    path.c_str(), res.rootEntity);
        return res.rootEntity;
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
        world.AddComponent<Visibility>     (e, Visibility{});
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
            const Entity tgt = resolveTargetIdx(cb.kv);
            if (tgt == NullEntity) continue;
            FollowSocketComponent fs;
            fs.target = world.MakeHandle(tgt);
            if (auto it = cb.kv.find("socket"); it != cb.kv.end())
                try { fs.socketIndex = static_cast<uint32_t>(std::stoul(it->second)); } catch (...) {}
            if (auto it = cb.kv.find("m"); it != cb.kv.end())
                fs.localOffset = ParseMatrix(it->second);
            world.AddComponent<FollowSocketComponent>(e, fs);
        }
        else if (cb.tag == "PrefabFollowEntity")
        {
            const Entity tgt = resolveTargetIdx(cb.kv);
            if (tgt == NullEntity) continue;
            FollowEntityComponent fe;
            fe.target = world.MakeHandle(tgt);
            if (auto it = cb.kv.find("m"); it != cb.kv.end())
                fe.localOffset = ParseMatrix(it->second);
            world.AddComponent<FollowEntityComponent>(e, fe);
        }
        else if (cb.tag == "PrefabSkeletonRef")
        {
            const Entity tgt = resolveTargetIdx(cb.kv);
            if (tgt == NullEntity) continue;
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
