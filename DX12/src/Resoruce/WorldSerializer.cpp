#include "Resource/WorldSerializer.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/PostProcessConfig.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/SkyboxComponent.h"
#include "Scene/SceneInstanceLoader.h"
#include "Graphics/Renderer.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
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

} // anonymous namespace

// ===========================================================================
// SaveWorld — generic component serialization via ComponentSerializerRegistry
// ===========================================================================
bool Resource::SaveWorld(World& world, const std::string& path,
                          const std::string& sceneName,
                          const std::string& postProcessConfigPath)
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
                          std::string* outPostProcessConfigPath)
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

    // 2. Clear existing world
    world.Clear();

    // 3. Parse text line by line
    std::string sceneName            = "Untitled";
    std::string postProcessConfigPath;
    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> compBlocks;
    int currentNodeIdx = -1;

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
        else if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
        {
            ComponentBlock cb;
            if (ParseComponentLine(line, currentNodeIdx, cb))
                compBlocks.push_back(std::move(cb));
        }
    }

    if (outSceneName) *outSceneName = sceneName;

    // 4. Pre-collect skinned node indices (SceneRef components)
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

    // 5. Create non-skinned entities (skinned roots are loaded from .iscn in step 7)
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
        world.AddComponent<Visibility>(e, Visibility{});
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

    // 6. Apply component blocks for non-skinned entities via registry
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

    // 7. Skinned character reconstruction from SceneRef/AnimRef
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

    LOG_SUCCESS("WorldSerializer: loaded '%s' — %zu entities, scene='%s'",
                path.c_str(), nodes.size(), sceneName.c_str());

    // 9. Apply referenced post-processing config (if any, and a Renderer is
    //    available). Missing file is non-fatal — worst case the pass keeps
    //    whatever state it was in before the load.
    if (outPostProcessConfigPath) *outPostProcessConfigPath = postProcessConfigPath;
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

    return true;
}
