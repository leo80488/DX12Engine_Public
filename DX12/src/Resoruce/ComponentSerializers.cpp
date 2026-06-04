#include "Resource/ComponentSerializers.h"
#include "Resource/SceneLoadContext.h"
#include "ECS/Components.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/TrailComponent.h"
#include "ECS/ParticleComponent.h"
#include "ECS/NotifyTypes.h"
#include "ECS/NotifySerialization.h"
#include <nlohmann/json.hpp>
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/CloudComponent.h"
#include "ECS/TODComponents.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/DDGIComponents.h"
#include "ECS/VolumeComponent.h"
#include "UI/UIComponents.h"
#include "UI/WorldSpaceUI.h"
#include "Physics/ChainPhysicsSystem.h"
#include "ECS/PhysicsComponents.h"     // RigidBodyComponent + ColliderComponent
#include "ECS/CharacterControllerComponent.h"
#include "ECS/PlayerComponent.h"
#include "ECS/AIIntentComponent.h"
#include "ECS/PerceptionComponent.h"
#include "Nav/NavComponents.h"
#include "ECS/FootIKComponent.h"       // FootIKComponent
#include "AI/AIComponents.h"           // AIComponent + BlackboardComponent
#include "Scripting/ScriptComponent.h"
#include "Resource/AssetManager.h"
#include "Resource/MaterialSerializer.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace DirectX;

// ---- Helpers ----------------------------------------------------------------

static std::string PercentEncode(const std::string& s)
{
    std::string r; r.reserve(s.size());
    for (char c : s) {
        if (c == '%') r += "%25";
        else if (c == ' ') r += "%20";
        else r += c;
    }
    return r;
}

static std::string PercentDecode(const std::string& s)
{
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
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

static float GetF(const KVMap& kv, const char* key, float def = 0.f)
{
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

static int GetI(const KVMap& kv, const char* key, int def = 0)
{
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

// 32-bit unsigned. std::stoul handles the full 0..0xFFFFFFFF range that bit
// masks (viewMask, RenderLayer.mask, etc.) need; stoi throws out_of_range on
// anything past INT_MAX and silently resets the value to def.
static uint32_t GetU(const KVMap& kv, const char* key, uint32_t def = 0)
{
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return static_cast<uint32_t>(std::stoul(it->second)); }
    catch (...) { return def; }
}

// ---- SceneLoadContext (legacy entity-idx remap) ----------------------------
//
// SceneSerializer binds the saved-idx → newEntity map for the duration of a
// load via SetLegacyEntityMap; migrated component deserializers call
// ResolveLegacyEntityIdx when they spot an old raw-id key (camEntity=42,
// followTarget=42, target=42).

namespace
{
    thread_local const std::unordered_map<int, Entity>* tls_legacyMap = nullptr;
}

namespace Resource
{
    void SetLegacyEntityMap(const std::unordered_map<int, Entity>* map)
    {
        tls_legacyMap = map;
    }

    Entity ResolveLegacyEntityIdx(uint32_t oldIdx)
    {
        if (!tls_legacyMap) return NullEntity;
        auto it = tls_legacyMap->find(static_cast<int>(oldIdx));
        return it != tls_legacyMap->end() ? it->second : NullEntity;
    }
}

// ---- AttachmentRef serialization helpers ------------------------------------
//
// Convention: every migrated entity-ref field writes "<name>Guid=<hex32>".
// On read, we accept either the new "<name>Guid" key (preferred) or the
// legacy raw "<name>" / variant key (back-compat) — the deserializer hands
// us the legacy uint, we resolve it via SceneLoadContext, then stamp a
// fresh GUID on the target so the next save is GUID-only.

namespace
{
    void WriteAttachmentRef(std::ostringstream& ss,
                            const char* keyPrefix,   // e.g. "target"
                            World& w,
                            const AttachmentRef& ref)
    {
        // Always write the GUID if we have one. ReverseFind from the cached
        // entity is a safety net if the ref was bound runtime but the GUID
        // wasn't filled.
        ECS::Guid g = ref.ownerGuid;
        if (!g.IsValid() && ref.cachedOwner != NullEntity)
            g = ECS::GuidRegistry::Get().ReverseFind(ref.cachedOwner);
        if (g.IsValid())
            ss << ' ' << keyPrefix << "Guid=" << g.ToHex();
    }

    // Read an AttachmentRef from the KV map. legacyKey is the old raw-id key
    // (or null if there was never a legacy form). Order of resolution:
    //   1. <keyPrefix>Guid present → parse, store, leave cache invalid.
    //   2. legacyKey present       → resolve via SceneLoadContext, stamp
    //                                GUID on target via BindAndStamp.
    //   3. neither                 → ref stays empty.
    void ReadAttachmentRef(const KVMap& kv,
                           const char* keyPrefix,
                           const char* legacyKey,
                           World& w,
                           Entity selfEntity,
                           AttachmentRef& out)
    {
        std::string guidKey = std::string(keyPrefix) + "Guid";
        if (auto it = kv.find(guidKey); it != kv.end())
        {
            out.ownerGuid        = ECS::Guid::FromHex(it->second);
            out.cachedOwner      = NullEntity;
            out.cachedGeneration = 0u;
            return;
        }
        if (legacyKey)
        {
            if (auto it = kv.find(legacyKey); it != kv.end())
            {
                uint32_t oldIdx = 0;
                try { oldIdx = static_cast<uint32_t>(std::stoul(it->second)); }
                catch (...) { return; }
                if (oldIdx == 0u) return;
                const Entity remapped = Resource::ResolveLegacyEntityIdx(oldIdx);
                if (remapped != NullEntity)
                {
                    // Stamp GUID on target so the next save is GUID-only.
                    // Self-reference is fine — Player.cameraEntity might
                    // legacy-point at a sibling that hasn't yet had a GUID.
                    out.BindAndStamp(w, remapped);
                }
            }
        }
    }
}

static std::string GetS(const KVMap& kv, const char* key, const std::string& def = "")
{
    auto it = kv.find(key);
    return (it != kv.end()) ? PercentDecode(it->second) : def;
}

// ---- Registration -----------------------------------------------------------

void RegisterAllComponentSerializers(ComponentSerializerRegistry& reg)
{
    // ==== LightData (INLINE) ====
    reg.Register(std::type_index(typeid(LightData)), {
        "LightData",
        [](World& w, Entity e) { return w.GetComponent<LightData>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ld = w.GetComponent<LightData>(e);
            if (!ld) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  LightData: type=%u radius=%.4f intensity=%.4f"
                " color=%.4f_%.4f_%.4f dir=%.4f_%.4f_%.4f spotAngle=%.4f"
                " castsShadow=%u\n",
                static_cast<uint32_t>(ld->type), ld->radius, ld->intensity,
                ld->color.x, ld->color.y, ld->color.z,
                ld->direction.x, ld->direction.y, ld->direction.z,
                ld->spotAngle,
                ld->castsShadow ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            LightData ld;
            ld.type        = static_cast<LightType>(GetI(kv, "type", 0));
            ld.radius      = GetF(kv, "radius", 10.f);
            ld.intensity   = GetF(kv, "intensity", 1.f);
            ld.spotAngle   = GetF(kv, "spotAngle", 0.5236f);
            ld.castsShadow = GetI(kv, "castsShadow", 0) != 0;
            // Parse color "r_g_b"
            auto cit = kv.find("color");
            if (cit != kv.end()) sscanf_s(cit->second.c_str(), "%f_%f_%f", &ld.color.x, &ld.color.y, &ld.color.z);
            auto dit = kv.find("dir");
            if (dit != kv.end()) sscanf_s(dit->second.c_str(), "%f_%f_%f", &ld.direction.x, &ld.direction.y, &ld.direction.z);
            w.AddComponent<LightData>(e, ld);
        }
    });

    // ==== VolumetricLightComponent (INLINE) ====
    // Opt-in marker so a Light entity also contributes to the volumetric fog
    // froxel grid. Previously unsaved → dragging a light out of a prefab /
    // sceneGraph into another scene lost its volumetric flag.
    reg.Register(std::type_index(typeid(VolumetricLightComponent)), {
        "VolumetricLight",
        [](World& w, Entity e) { return w.GetComponent<VolumetricLightComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* vl = w.GetComponent<VolumetricLightComponent>(e);
            if (!vl) return;
            char buf[128];
            snprintf(buf, sizeof(buf),
                "  VolumetricLight: enabled=%u intensityScale=%.4f\n",
                vl->enabled ? 1u : 0u, vl->intensityScale);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            VolumetricLightComponent vl;
            vl.enabled        = GetI(kv, "enabled", 1) != 0;
            vl.intensityScale = GetF(kv, "intensityScale", 1.f);
            w.AddComponent<VolumetricLightComponent>(e, vl);
        }
    });

    // ==== ReflectionProbeComponent (INLINE) ====
    // Persists the artist-set inner/outer half-extents so a saved scene
    // restores probe placement bounds exactly. cubemapSlice is runtime-only
    // (Renderer reassigns on world load) and the BAKED flag is dropped — but
    // bakedCubemap (the exported .itex path) IS persisted: Renderer::Export-
    // BakedProbeCubemaps stamps it during SaveScene and BuildScene_Upload-
    // Probes restores the cubemap straight off disk on the next load, so a
    // non-realtime probe doesn't pay a full 6-face re-bake every load.
    reg.Register(std::type_index(typeid(ReflectionProbeComponent)), {
        "ReflectionProbe",
        [](World& w, Entity e) { return w.GetComponent<ReflectionProbeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* rp = w.GetComponent<ReflectionProbeComponent>(e);
            if (!rp) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  ReflectionProbe: innerExtents=%.4f,%.4f,%.4f outerExtents=%.4f,%.4f,%.4f"
                " realtime=%u tickIntervalFrames=%u",
                rp->innerExtents.x, rp->innerExtents.y, rp->innerExtents.z,
                rp->outerExtents.x, rp->outerExtents.y, rp->outerExtents.z,
                rp->realtime ? 1u : 0u, rp->tickIntervalFrames);
            ss << buf;
            // Path is percent-encoded so spaces / '%' survive the KV line.
            if (!rp->bakedCubemapPath.empty())
                ss << " bakedCubemap=" << PercentEncode(rp->bakedCubemapPath);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ReflectionProbeComponent rp;
            // Parse "x,y,z" from the value string. Falls back to defaults if
            // the key is missing (component default-construct).
            auto parseVec3 = [&](const char* key, XMFLOAT3& out) {
                auto it = kv.find(key);
                if (it == kv.end()) return;
                float x = out.x, y = out.y, z = out.z;
                if (sscanf_s(it->second.c_str(), "%f,%f,%f", &x, &y, &z) >= 1)
                    out = { x, y, z };
            };
            parseVec3("innerExtents", rp.innerExtents);
            parseVec3("outerExtents", rp.outerExtents);
            rp.realtime           = GetI(kv, "realtime", 0) != 0;
            rp.tickIntervalFrames = static_cast<uint32_t>(GetI(kv, "tickIntervalFrames", 60));
            auto bc = kv.find("bakedCubemap");
            if (bc != kv.end())
                rp.bakedCubemapPath = PercentDecode(bc->second);
            w.AddComponent<ReflectionProbeComponent>(e, rp);
        }
    });

    // ==== DDGIVolumeComponent (INLINE) ====
    // Authored DDGI volume params. The matching DDGIVolumeRuntimeComponent
    // (GPU resource handles) is intentionally NOT serialized — Renderer's
    // BuildScene_UpdateDDGI auto-promotes it on the next tick after a
    // DDGIVolumeComponent is added without one. Field keys mirror the struct
    // field names where short, but use 2-letter prefixes for vec3 components
    // (ox/oy/oz, ex/ey/ez, tx/ty/tz) to match the rest of this file.
    reg.Register(std::type_index(typeid(DDGIVolumeComponent)), {
        "DDGIVolume",
        [](World& w, Entity e) { return w.GetComponent<DDGIVolumeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* v = w.GetComponent<DDGIVolumeComponent>(e);
            if (!v) return;
            char buf[640];
            snprintf(buf, sizeof(buf),
                "  DDGIVolume: ox=%.4f oy=%.4f oz=%.4f"
                " ex=%.4f ey=%.4f ez=%.4f"
                " px=%u py=%u pz=%u rays=%u"
                " hyst=%.4f nb=%.4f vb=%.4f bfr=%.4f"
                // NOTE: debugDraw is intentionally NOT serialized — it is an
                // editor-only debug toggle, not gameplay/authoring data.
                " reloc=%u classify=%u prio=%d cascade=%u"
                " tx=%.4f ty=%.4f tz=%.4f tscale=%.4f\n",
                v->origin.x, v->origin.y, v->origin.z,
                v->extent.x, v->extent.y, v->extent.z,
                v->probeCountsX, v->probeCountsY, v->probeCountsZ, v->raysPerProbe,
                v->hysteresis, v->normalBias, v->viewBias, v->boundaryFadeRatio,
                v->enableRelocation     ? 1u : 0u,
                v->enableClassification ? 1u : 0u,
                v->priority, v->cascadeLevel,
                v->diffuseTint.x, v->diffuseTint.y, v->diffuseTint.z, v->diffuseScale);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            DDGIVolumeComponent v;
            v.origin = { GetF(kv, "ox", v.origin.x), GetF(kv, "oy", v.origin.y), GetF(kv, "oz", v.origin.z) };
            v.extent = { GetF(kv, "ex", v.extent.x), GetF(kv, "ey", v.extent.y), GetF(kv, "ez", v.extent.z) };
            v.probeCountsX = static_cast<uint32_t>(GetI(kv, "px",   static_cast<int>(v.probeCountsX)));
            v.probeCountsY = static_cast<uint32_t>(GetI(kv, "py",   static_cast<int>(v.probeCountsY)));
            v.probeCountsZ = static_cast<uint32_t>(GetI(kv, "pz",   static_cast<int>(v.probeCountsZ)));
            v.raysPerProbe = static_cast<uint32_t>(GetI(kv, "rays", static_cast<int>(v.raysPerProbe)));
            v.hysteresis        = GetF(kv, "hyst", v.hysteresis);
            v.normalBias        = GetF(kv, "nb",   v.normalBias);
            v.viewBias          = GetF(kv, "vb",   v.viewBias);
            v.boundaryFadeRatio = GetF(kv, "bfr",  v.boundaryFadeRatio);
            v.enableRelocation     = GetI(kv, "reloc",    v.enableRelocation     ? 1 : 0) != 0;
            v.enableClassification = GetI(kv, "classify", v.enableClassification ? 1 : 0) != 0;
            v.priority      = GetI(kv, "prio",    v.priority);
            v.cascadeLevel  = static_cast<uint32_t>(GetI(kv, "cascade", static_cast<int>(v.cascadeLevel)));
            // debugDraw is editor-only and no longer persisted (legacy "dbg="
            // tokens in old scenes are simply ignored → defaults to false).
            v.diffuseTint   = { GetF(kv, "tx", v.diffuseTint.x),
                                GetF(kv, "ty", v.diffuseTint.y),
                                GetF(kv, "tz", v.diffuseTint.z) };
            v.diffuseScale  = GetF(kv, "tscale", v.diffuseScale);
            w.AddComponent<DDGIVolumeComponent>(e, v);
        }
    });

    // ==== DDGISceneTagComponent (INLINE — TLAS-membership marker) ====
    // Empty payload: presence is the meaningful state. Reserved field is
    // emitted for forward compat in case future per-entity tweaks land.
    reg.Register(std::type_index(typeid(DDGISceneTagComponent)), {
        "DDGISceneTag",
        [](World& w, Entity e) { return w.GetComponent<DDGISceneTagComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* t = w.GetComponent<DDGISceneTagComponent>(e);
            if (!t) return;
            char buf[64];
            snprintf(buf, sizeof(buf), "  DDGISceneTag: reserved=%u\n",
                     static_cast<unsigned>(t->reserved));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            DDGISceneTagComponent t;
            t.reserved = static_cast<uint8_t>(GetI(kv, "reserved", 0));
            w.AddComponent<DDGISceneTagComponent>(e, t);
        }
    });

    // ==== IndirectLightingSettingsComponent (INLINE — scene-singleton) ====
    // Renderer::BuildScene_UpdateDDGI copies the first instance found in the
    // pool into m_ddgiSettings each frame, so a single entity carrying this
    // component is enough to drive engine-wide indirect-lighting knobs.
    reg.Register(std::type_index(typeid(IndirectLightingSettingsComponent)), {
        "IndirectLighting",
        [](World& w, Entity e) { return w.GetComponent<IndirectLightingSettingsComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* s = w.GetComponent<IndirectLightingSettingsComponent>(e);
            if (!s) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  IndirectLighting: ddgi=%u ddgiScale=%.4f skyScale=%.4f"
                " ddgiAONear=%.4f ssr=%u ssrCut=%.4f ssrEdge=%.4f"
                " probePri=%u ddgiRoughSpec=%u\n",
                s->ddgiEnabled ? 1u : 0u,
                s->ddgiDiffuseScale,
                s->skyIBLDiffuseScale,
                s->ddgiAONearFieldStrength,
                s->ssrEnabled  ? 1u : 0u,
                s->ssrRoughnessCutoff,
                s->ssrEdgeFadeRatio,
                s->reflectionProbePriorityOverDDGI ? 1u : 0u,
                s->useDDGIForRoughSpecularFallback ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            IndirectLightingSettingsComponent s;
            s.ddgiEnabled              = GetI(kv, "ddgi",        s.ddgiEnabled ? 1 : 0) != 0;
            s.ddgiDiffuseScale         = GetF(kv, "ddgiScale",   s.ddgiDiffuseScale);
            s.skyIBLDiffuseScale       = GetF(kv, "skyScale",    s.skyIBLDiffuseScale);
            s.ddgiAONearFieldStrength  = GetF(kv, "ddgiAONear",  s.ddgiAONearFieldStrength);
            s.ssrEnabled               = GetI(kv, "ssr",         s.ssrEnabled  ? 1 : 0) != 0;
            s.ssrRoughnessCutoff       = GetF(kv, "ssrCut",      s.ssrRoughnessCutoff);
            s.ssrEdgeFadeRatio         = GetF(kv, "ssrEdge",     s.ssrEdgeFadeRatio);
            s.reflectionProbePriorityOverDDGI =
                GetI(kv, "probePri",      s.reflectionProbePriorityOverDDGI ? 1 : 0) != 0;
            s.useDDGIForRoughSpecularFallback =
                GetI(kv, "ddgiRoughSpec", s.useDDGIForRoughSpecularFallback ? 1 : 0) != 0;
            w.AddComponent<IndirectLightingSettingsComponent>(e, s);
        }
    });

    // ==== ECS::VolumeComponent (INLINE) ====
    // Post-process spatial volume. Persists every Volume field + per-stage
    // override flag + override payload when present. Single-line key=value
    // format like every other INLINE component. On read, each std::optional
    // is populated only when its `hasXxx=1` flag is set.
    reg.Register(std::type_index(typeid(ECS::VolumeComponent)), {
        "PPVolume",
        [](World& w, Entity e) { return w.GetComponent<ECS::VolumeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* vc = w.GetComponent<ECS::VolumeComponent>(e);
            if (!vc) return;
            const PostProcess::Volume& v = vc->volume;

            char buf[512];
            ss << "  PPVolume:";

            snprintf(buf, sizeof(buf),
                " enabled=%u shape=%u priority=%d blend=%.4f"
                " cx=%.4f cy=%.4f cz=%.4f"
                " ex=%.4f ey=%.4f ez=%.4f"
                " label=%s"
                " hasCAS=%u hasAE=%u hasBloom=%u hasTM=%u",
                v.enabled ? 1u : 0u,
                static_cast<unsigned>(v.shape),
                v.priority, v.blendDistance,
                v.center.x, v.center.y, v.center.z,
                v.extents.x, v.extents.y, v.extents.z,
                PercentEncode(std::string(v.label)).c_str(),
                v.override.cas.has_value()          ? 1u : 0u,
                v.override.autoExposure.has_value() ? 1u : 0u,
                v.override.bloom.has_value()        ? 1u : 0u,
                v.override.tonemapping.has_value()  ? 1u : 0u);
            ss << buf;

            if (v.override.cas)
            {
                const auto& c = *v.override.cas;
                snprintf(buf, sizeof(buf),
                    " casEn=%u casSharp=%.4f",
                    c.enabled ? 1u : 0u, c.sharpness);
                ss << buf;
            }

            if (v.override.autoExposure)
            {
                const auto& ae = *v.override.autoExposure;
                snprintf(buf, sizeof(buf),
                    " aeEn=%u aeManual=%.4f aeTau=%.4f"
                    " aeMinL=%.4f aeMaxL=%.4f aeLowP=%.4f aeHighP=%.4f"
                    " aeMinE=%.4f aeMaxE=%.4f aeEV=%.4f aeKey=%.4f",
                    ae.enabled ? 1u : 0u, ae.manualExposure, ae.adaptationTau,
                    ae.minLogLuma, ae.maxLogLuma, ae.lowPercent, ae.highPercent,
                    ae.minExposure, ae.maxExposure, ae.evBias, ae.keyValue);
                ss << buf;
            }

            // Bloom has no user-facing fields yet — the flag alone is enough
            // for round-trip (marks "override is enabled but empty").

            if (v.override.tonemapping)
            {
                const auto& tm = *v.override.tonemapping;
                const auto& g  = tm.grading;
                // Two chunks to stay inside buf[512].
                snprintf(buf, sizeof(buf),
                    " tmBloom=%.4f tmGradEn=%u"
                    " tmExp=%.4f tmCon=%.4f tmBri=%.4f"
                    " tmLiftR=%.4f tmLiftG=%.4f tmLiftB=%.4f"
                    " tmGamR=%.4f tmGamG=%.4f tmGamB=%.4f",
                    tm.bloomStrength, tm.colorGradingEnabled ? 1u : 0u,
                    g.exposure, g.contrast, g.brightness,
                    g.lift.x, g.lift.y, g.lift.z,
                    g.gamma.x, g.gamma.y, g.gamma.z);
                ss << buf;
                snprintf(buf, sizeof(buf),
                    " tmGainR=%.4f tmGainG=%.4f tmGainB=%.4f"
                    " tmHue=%.4f tmSat=%.4f tmVib=%.4f"
                    " tmTemp=%.4f tmTint=%.4f tmVig=%.4f tmGrain=%.4f",
                    g.gain.x, g.gain.y, g.gain.z,
                    g.hueShift, g.saturation, g.vibrance,
                    g.temperature, g.tint,
                    g.vignetteStrength, g.filmGrain);
                ss << buf;
            }

            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ECS::VolumeComponent vc;
            PostProcess::Volume& v = vc.volume;

            v.enabled       = GetI(kv, "enabled",  1) != 0;
            v.shape         = static_cast<PostProcess::VolumeShape>(
                                  GetI(kv, "shape", 0));
            v.priority      = GetI(kv, "priority", 0);
            v.blendDistance = GetF(kv, "blend",    1.0f);
            v.center.x      = GetF(kv, "cx",       0.0f);
            v.center.y      = GetF(kv, "cy",       0.0f);
            v.center.z      = GetF(kv, "cz",       0.0f);
            v.extents.x     = GetF(kv, "ex",       1.0f);
            v.extents.y     = GetF(kv, "ey",       1.0f);
            v.extents.z     = GetF(kv, "ez",       1.0f);

            const std::string label = GetS(kv, "label", "");
            if (!label.empty())
            {
                // Null-terminated truncated copy — sizeof(v.label) includes
                // the terminator.
                const size_t n = std::min<size_t>(label.size(), sizeof(v.label) - 1);
                std::memcpy(v.label, label.data(), n);
                v.label[n] = '\0';
            }

            if (GetI(kv, "hasCAS", 0))
            {
                PostProcess::CASParams p;
                p.enabled   = GetI(kv, "casEn",    1) != 0;
                p.sharpness = GetF(kv, "casSharp", 0.6f);
                v.override.cas = p;
            }

            if (GetI(kv, "hasAE", 0))
            {
                PostProcess::AutoExposureParams p;
                p.enabled        = GetI(kv, "aeEn",    1) != 0;
                p.manualExposure = GetF(kv, "aeManual", 1.0f);
                p.adaptationTau  = GetF(kv, "aeTau",    1.5f);
                p.minLogLuma     = GetF(kv, "aeMinL",  -5.0f);
                p.maxLogLuma     = GetF(kv, "aeMaxL",   3.5f);
                p.lowPercent     = GetF(kv, "aeLowP",   0.50f);
                p.highPercent    = GetF(kv, "aeHighP",  0.85f);
                p.minExposure    = GetF(kv, "aeMinE",   0.10f);
                p.maxExposure    = GetF(kv, "aeMaxE",   8.00f);
                p.evBias         = GetF(kv, "aeEV",     0.00f);
                p.keyValue       = GetF(kv, "aeKey",    0.18f);
                v.override.autoExposure = p;
            }

            if (GetI(kv, "hasBloom", 0))
            {
                v.override.bloom = PostProcess::BloomParams{};
            }

            if (GetI(kv, "hasTM", 0))
            {
                PostProcess::TonemappingParams p;
                p.bloomStrength       = GetF(kv, "tmBloom",  0.04f);
                p.colorGradingEnabled = GetI(kv, "tmGradEn", 1) != 0;
                auto& g = p.grading;
                g.exposure   = GetF(kv, "tmExp", 0.0f);
                g.contrast   = GetF(kv, "tmCon", 1.0f);
                g.brightness = GetF(kv, "tmBri", 0.0f);
                g.lift.x     = GetF(kv, "tmLiftR", 0.0f);
                g.lift.y     = GetF(kv, "tmLiftG", 0.0f);
                g.lift.z     = GetF(kv, "tmLiftB", 0.0f);
                g.gamma.x    = GetF(kv, "tmGamR",  1.0f);
                g.gamma.y    = GetF(kv, "tmGamG",  1.0f);
                g.gamma.z    = GetF(kv, "tmGamB",  1.0f);
                g.gain.x     = GetF(kv, "tmGainR", 1.0f);
                g.gain.y     = GetF(kv, "tmGainG", 1.0f);
                g.gain.z     = GetF(kv, "tmGainB", 1.0f);
                g.hueShift         = GetF(kv, "tmHue",  0.0f);
                g.saturation       = GetF(kv, "tmSat",  1.0f);
                g.vibrance         = GetF(kv, "tmVib",  0.0f);
                g.temperature      = GetF(kv, "tmTemp", 0.0f);
                g.tint             = GetF(kv, "tmTint", 0.0f);
                g.vignetteStrength = GetF(kv, "tmVig",  0.0f);
                g.filmGrain        = GetF(kv, "tmGrain", 0.0f);
                v.override.tonemapping = p;
            }

            w.AddComponent<ECS::VolumeComponent>(e, vc);
        }
    });

    // ==== BillboardComponent (INLINE) ====
    reg.Register(std::type_index(typeid(BillboardComponent)), {
        "Billboard",
        [](World& w, Entity e) { return w.GetComponent<BillboardComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* bb = w.GetComponent<BillboardComponent>(e);
            if (!bb) return;
            char buf[128];
            snprintf(buf, sizeof(buf), "  Billboard: mode=%u worldSize=%.4f\n",
                     static_cast<uint32_t>(bb->mode), bb->worldSize);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            BillboardComponent bb;
            bb.mode      = static_cast<BillboardMode>(GetI(kv, "mode", 2));
            bb.worldSize = GetF(kv, "worldSize", 1.f);
            w.AddComponent<BillboardComponent>(e, bb);
        }
    });

    // ==== TrailComponent (INLINE) ====
    // Ribbon-trail visual + sampling params. trailSlot, lastSamplePos, and
    // hasLastSample are runtime CPU state managed by TrailSystem on first
    // sight, so they are intentionally not persisted.
    reg.Register(std::type_index(typeid(TrailComponent)), {
        "Trail",
        [](World& w, Entity e) { return w.GetComponent<TrailComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* tc = w.GetComponent<TrailComponent>(e);
            if (!tc) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  Trail: enabled=%u width=%.4f maxAge=%.4f minSampleDistance=%.4f"
                " startColor=%.4f_%.4f_%.4f_%.4f endColor=%.4f_%.4f_%.4f_%.4f\n",
                tc->enabled ? 1u : 0u, tc->width, tc->maxAge, tc->minSampleDistance,
                tc->startColor.x, tc->startColor.y, tc->startColor.z, tc->startColor.w,
                tc->endColor.x,   tc->endColor.y,   tc->endColor.z,   tc->endColor.w);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            TrailComponent tc;
            tc.enabled           = GetI(kv, "enabled", 1) != 0;
            tc.width             = GetF(kv, "width", 0.15f);
            tc.maxAge            = GetF(kv, "maxAge", 1.5f);
            tc.minSampleDistance = GetF(kv, "minSampleDistance", 0.02f);
            auto sit = kv.find("startColor");
            if (sit != kv.end())
                sscanf_s(sit->second.c_str(), "%f_%f_%f_%f",
                         &tc.startColor.x, &tc.startColor.y, &tc.startColor.z, &tc.startColor.w);
            auto eit = kv.find("endColor");
            if (eit != kv.end())
                sscanf_s(eit->second.c_str(), "%f_%f_%f_%f",
                         &tc.endColor.x, &tc.endColor.y, &tc.endColor.z, &tc.endColor.w);
            w.AddComponent<TrailComponent>(e, tc);
        }
    });

    // ==== ParticleEmitterComponent (INLINE — GPU emitter authored params) ====
    // textureBindlessIdx / textureGpuHandle / spawnAccumulator are runtime CPU
    // state — ParticleSystem re-resolves the bindless slot from texturePath on
    // first tick after load, so we only persist the path itself.
    reg.Register(std::type_index(typeid(ParticleEmitterComponent)), {
        "ParticleEmitter",
        [](World& w, Entity e) { return w.GetComponent<ParticleEmitterComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<ParticleEmitterComponent>(e);
            if (!p) return;
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "  ParticleEmitter: enabled=%u spawnRate=%.4f startLifetime=%.4f startSize=%.4f"
                " velMin=%.4f_%.4f_%.4f velMax=%.4f_%.4f_%.4f"
                " startColor=%.4f_%.4f_%.4f_%.4f endColor=%.4f_%.4f_%.4f_%.4f"
                " gravity=%.4f_%.4f_%.4f"
                " blendMode=%u visualMode=%u"
                " shape=%u"
                " sphereRadius=%.4f sphereShell=%u"
                " coneHalfAngle=%.4f coneDir=%.4f_%.4f_%.4f coneLength=%.4f"
                " boxHalfExtents=%.4f_%.4f_%.4f"
                " circleRadius=%.4f circleNormal=%.4f_%.4f_%.4f"
                " meshSourceEntity=%u",
                p->enabled ? 1u : 0u, p->spawnRate, p->startLifetime, p->startSize,
                p->velocityMin.x, p->velocityMin.y, p->velocityMin.z,
                p->velocityMax.x, p->velocityMax.y, p->velocityMax.z,
                p->startColor.x, p->startColor.y, p->startColor.z, p->startColor.w,
                p->endColor.x,   p->endColor.y,   p->endColor.z,   p->endColor.w,
                p->gravity.x, p->gravity.y, p->gravity.z,
                static_cast<uint32_t>(p->blendMode),
                static_cast<uint32_t>(p->visualMode),
                static_cast<uint32_t>(p->shape),
                p->sphereRadius, p->sphereSpawnOnShell ? 1u : 0u,
                p->coneHalfAngle, p->coneDirection.x, p->coneDirection.y, p->coneDirection.z, p->coneLength,
                p->boxHalfExtents.x, p->boxHalfExtents.y, p->boxHalfExtents.z,
                p->circleRadius, p->circleNormal.x, p->circleNormal.y, p->circleNormal.z,
                static_cast<uint32_t>(p->meshSourceEntity));
            ss << buf;
            // Texture path is percent-encoded so spaces / '%' survive the KV line.
            if (!p->texturePath.empty())
                ss << " texturePath=" << PercentEncode(p->texturePath);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ParticleEmitterComponent p;
            p.enabled        = GetI(kv, "enabled", 1) != 0;
            p.spawnRate      = GetF(kv, "spawnRate",     p.spawnRate);
            p.startLifetime  = GetF(kv, "startLifetime", p.startLifetime);
            p.startSize      = GetF(kv, "startSize",     p.startSize);

            auto parseVec3 = [&](const char* key, XMFLOAT3& out) {
                auto it = kv.find(key);
                if (it == kv.end()) return;
                sscanf_s(it->second.c_str(), "%f_%f_%f", &out.x, &out.y, &out.z);
            };
            auto parseVec4 = [&](const char* key, XMFLOAT4& out) {
                auto it = kv.find(key);
                if (it == kv.end()) return;
                sscanf_s(it->second.c_str(), "%f_%f_%f_%f", &out.x, &out.y, &out.z, &out.w);
            };
            parseVec3("velMin",         p.velocityMin);
            parseVec3("velMax",         p.velocityMax);
            parseVec4("startColor",     p.startColor);
            parseVec4("endColor",       p.endColor);
            parseVec3("gravity",        p.gravity);
            parseVec3("coneDir",        p.coneDirection);
            parseVec3("boxHalfExtents", p.boxHalfExtents);
            parseVec3("circleNormal",   p.circleNormal);

            p.blendMode  = static_cast<ParticleBlendMode>(GetI(kv, "blendMode",
                                static_cast<int>(p.blendMode)));
            p.visualMode = static_cast<ParticleVisualMode>(GetI(kv, "visualMode",
                                static_cast<int>(p.visualMode)));
            p.shape      = static_cast<ParticleShape>(GetI(kv, "shape",
                                static_cast<int>(p.shape)));

            p.sphereRadius       = GetF(kv, "sphereRadius",  p.sphereRadius);
            p.sphereSpawnOnShell = GetI(kv, "sphereShell",   p.sphereSpawnOnShell ? 1 : 0) != 0;
            p.coneHalfAngle      = GetF(kv, "coneHalfAngle", p.coneHalfAngle);
            p.coneLength         = GetF(kv, "coneLength",    p.coneLength);
            p.circleRadius       = GetF(kv, "circleRadius",  p.circleRadius);
            p.meshSourceEntity   = GetU(kv, "meshSourceEntity", p.meshSourceEntity);

            // texturePath stays as a path; ParticleSystem resolves the
            // bindless slot + GPU handle on its next tick after load.
            p.texturePath        = GetS(kv, "texturePath");
            p.textureBindlessIdx = -1;
            p.textureGpuHandle   = 0;

            w.AddComponent<ParticleEmitterComponent>(e, p);
        }
    });

    // ==== TimelineComponent (INLINE — AnimNotify tracks) ====
    // Nested structure (tracks → notifies + states → PropertyBag) doesn't
    // fit the flat KV format other components use, so we encode the whole
    // payload as a single percent-encoded JSON blob under key "tl". The
    // KV-line parser splits on whitespace and tolerates arbitrary value
    // content as long as it contains no unencoded spaces or '=' inside it.
    // Runtime CPU state (lastObservedTime) is reset on load — TimelineSystem
    // snapshots the current animation time on first sight.
    reg.Register(std::type_index(typeid(TimelineComponent)), {
        "Timeline",
        [](World& w, Entity e) {
            const auto* tl = w.GetComponent<TimelineComponent>(e);
            // Skip empty timelines so an editor-created shell doesn't bloat
            // every scene file. The runtime auto-creates these on demand.
            return tl && !tl->tracks.empty();
        },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* tl = w.GetComponent<TimelineComponent>(e);
            if (!tl) return;
            // Schema is shared with the .ianim notify section (NotifyIO) so a
            // clip-authored notify set and an entity timeline are byte-for-byte
            // interchangeable. See ECS/NotifySerialization.h.
            const std::string payload =
                NotifyIO::TracksToJsonString(tl->tracks, tl->clipDuration, tl->nextNotifyId);
            ss << "  Timeline: tl=" << PercentEncode(payload) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            const auto it = kv.find("tl");
            if (it == kv.end()) return;

            TimelineComponent tl;
            // lastObservedTime stays -1 → TimelineSystem snapshots on first
            // sight without firing every notify in the clip.
            if (!NotifyIO::TracksFromJsonString(PercentDecode(it->second),
                                                tl.tracks, &tl.clipDuration, &tl.nextNotifyId))
                return; // malformed payload — skip rather than add a broken component
            w.AddComponent<TimelineComponent>(e, std::move(tl));
        }
    });

    // ==== CameraComponent (INLINE — lens / view-projection only) ====
    // The camera pose lives on LocalTransform (N line) and its FPS-controller
    // state on the separate CameraController block below.
    reg.Register(std::type_index(typeid(CameraComponent)), {
        "Camera",
        [](World& w, Entity e) { return w.GetComponent<CameraComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<CameraComponent>(e);
            if (!c) return;
            char buf[128];
            snprintf(buf, sizeof(buf),
                "  Camera: fov=%.4f nearZ=%.4f farZ=%.4f\n",
                c->fov, c->nearZ, c->farZ);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CameraComponent c;
            c.fov   = GetF(kv, "fov", 1.047f);
            c.nearZ = GetF(kv, "nearZ", 0.1f);
            c.farZ  = GetF(kv, "farZ", 200.f);
            w.AddComponent<CameraComponent>(e, c);

            // ---- Legacy migration ------------------------------------------
            // Pre-split saves stored the camera pose + FPS-controller fields
            // inline on the Camera line. New saves keep the pose on
            // LocalTransform (N line) and controller fields on a separate
            // CameraController line. Reconstruct them if the legacy keys exist.
            if (kv.count("px") || kv.count("yaw"))
            {
                if (!w.GetComponent<CameraControllerComponent>(e))
                {
                    CameraControllerComponent ctrl;
                    ctrl.yaw       = GetF(kv, "yaw",       -2.47f);
                    ctrl.pitch     = GetF(kv, "pitch",      0.44f);
                    ctrl.moveSpeed = GetF(kv, "moveSpeed", 10.f);
                    w.AddComponent<CameraControllerComponent>(e, ctrl);
                }
                if (auto* lt = w.GetComponent<LocalTransform>(e))
                {
                    lt->translation = { GetF(kv,"px",4.f), GetF(kv,"py",3.f), GetF(kv,"pz",5.f) };
                    XMStoreFloat4(&lt->rotation,
                        XMQuaternionRotationRollPitchYaw(
                            GetF(kv,"pitch",0.44f), GetF(kv,"yaw",-2.47f), 0.f));
                }
            }
        }
    });

    // ==== GuidComponent (INLINE — stable cross-session identity) ====
    // Must come BEFORE any component that references GUIDs by entity, so
    // when load reaches a serialised AttachmentRef the target's GUID is
    // already on the target entity. SceneSerializer guarantees this by
    // iterating cb blocks in file order; we additionally rebuild the
    // GuidRegistry from the world after the deserialize phase as a safety
    // net for arbitrary order.
    reg.Register(std::type_index(typeid(GuidComponent)), {
        "Guid",
        [](World& w, Entity e) { return w.GetComponent<GuidComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* gc = w.GetComponent<GuidComponent>(e);
            if (!gc || !gc->guid.IsValid()) return;
            ss << "  Guid: hex=" << gc->guid.ToHex() << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            GuidComponent gc{};
            auto it = kv.find("hex");
            if (it != kv.end()) gc.guid = ECS::Guid::FromHex(it->second);
            if (!gc.guid.IsValid()) gc.guid = ECS::Guid::Generate();
            w.AddComponent<GuidComponent>(e, gc);
            // Register immediately so any later component on the same load
            // pass can resolve a ref to this entity even before
            // RebuildFromWorld runs.
            ECS::GuidRegistry::Get().Register(gc.guid, e);
        }
    });

    // ==== PersistentTag (INLINE — save-game opt-in marker) ====
    // Empty body; presence alone signals "save this entity to the save file
    // in addition to the scene". Save-game writer isn't built yet but the
    // tag is here so authors can mark entities now.
    reg.Register(std::type_index(typeid(PersistentTag)), {
        "Persistent",
        [](World& w, Entity e) { return w.GetComponent<PersistentTag>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            if (w.GetComponent<PersistentTag>(e))
                ss << "  Persistent:\n";
        },
        [](World& w, Entity e, const KVMap& /*kv*/, Resource::AssetManager*) {
            if (!w.HasComponent<PersistentTag>(e))
                w.AddComponent<PersistentTag>(e, PersistentTag{});
        }
    });

    // ==== CameraControllerComponent (INLINE — FPS controller state) ====
    // mode + followTarget joined in here. followTarget rides as a GUID
    // (followTargetGuid=…); legacy raw "followTarget=42" reads via
    // SceneLoadContext for back-compat.
    reg.Register(std::type_index(typeid(CameraControllerComponent)), {
        "CameraController",
        [](World& w, Entity e) { return w.GetComponent<CameraControllerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<CameraControllerComponent>(e);
            if (!c) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  CameraController: yaw=%.4f pitch=%.4f"
                " mouseSensitivity=%.5f moveSpeed=%.4f mode=%d"
                " tpDist=%.3f hx=%.3f hy=%.3f hz=%.3f"
                " sx=%.3f sy=%.3f sz=%.3f camCol=%d camProbe=%.3f"
                " fLag=%.4f dLag=%.4f",
                c->yaw, c->pitch, c->mouseSensitivity, c->moveSpeed,
                (int)c->mode,
                c->thirdPersonDistance,
                c->headOffset.x, c->headOffset.y, c->headOffset.z,
                c->shoulderOffset.x, c->shoulderOffset.y, c->shoulderOffset.z,
                (int)c->cameraCollisionEnabled, c->cameraProbeRadius,
                c->followLag, c->distanceLag);
            ss << buf;
            WriteAttachmentRef(ss, "followTarget", w, c->followTarget);
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CameraControllerComponent c;
            c.yaw              = GetF(kv, "yaw",              -2.47f);
            c.pitch            = GetF(kv, "pitch",             0.44f);
            c.mouseSensitivity = GetF(kv, "mouseSensitivity",  0.003f);
            c.moveSpeed        = GetF(kv, "moveSpeed",        10.f);
            c.mode             = static_cast<CameraControllerComponent::Mode>(
                                     GetI(kv, "mode", (int)CameraControllerComponent::Mode::Free));
            c.thirdPersonDistance     = GetF(kv, "tpDist",   4.f);
            c.headOffset              = { GetF(kv,"hx",0.f), GetF(kv,"hy",1.65f), GetF(kv,"hz",0.f) };
            c.shoulderOffset          = { GetF(kv,"sx",0.30f), GetF(kv,"sy",1.60f), GetF(kv,"sz",0.f) };
            c.cameraCollisionEnabled  = GetI(kv, "camCol",   1) != 0;
            c.cameraProbeRadius       = GetF(kv, "camProbe", 0.20f);
            c.followLag               = GetF(kv, "fLag",     0.0f);
            c.distanceLag             = GetF(kv, "dLag",     0.0f);
            ReadAttachmentRef(kv, "followTarget", "followTarget", w, e, c.followTarget);
            w.AddComponent<CameraControllerComponent>(e, c);
        }
    });

    // ==== FollowCameraComponent (INLINE — camera-stack VCam behavior) ====
    // Drives a VCam to chase `target`. target is an AttachmentRef so it
    // survives scene reload. Was not previously serialized at all —
    // FollowCam authoring was runtime-only before.
    reg.Register(std::type_index(typeid(FollowCameraComponent)), {
        "FollowCamera",
        [](World& w, Entity e) { return w.GetComponent<FollowCameraComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* fc = w.GetComponent<FollowCameraComponent>(e);
            if (!fc) return;
            char buf[224];
            snprintf(buf, sizeof(buf),
                "  FollowCamera: ox=%.3f oy=%.3f oz=%.3f"
                " lx=%.3f ly=%.3f lz=%.3f damp=%.3f rdamp=%.3f lookAt=%d",
                fc->offset.x, fc->offset.y, fc->offset.z,
                fc->lookAtOffset.x, fc->lookAtOffset.y, fc->lookAtOffset.z,
                fc->damping, fc->rotationDamping, (int)fc->useLookAt);
            ss << buf;
            WriteAttachmentRef(ss, "target", w, fc->target);
            ss << '\n';
            // Need a FollowCameraTag for the Follow tick system to drive
            // this VCam — serialize the tag's presence alongside.
            if (w.HasComponent<FollowCameraTag>(e))
                ss << "  FollowCameraTag:\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            FollowCameraComponent fc{};
            fc.offset          = { GetF(kv,"ox",0.f),  GetF(kv,"oy",1.6f),  GetF(kv,"oz",-4.f) };
            fc.lookAtOffset    = { GetF(kv,"lx",0.f),  GetF(kv,"ly",1.5f),  GetF(kv,"lz",0.f) };
            fc.damping         = GetF(kv, "damp",    8.f);
            fc.rotationDamping = GetF(kv, "rdamp",   8.f);
            fc.useLookAt       = GetI(kv, "lookAt",  1) != 0;
            ReadAttachmentRef(kv, "target", "target", w, e, fc.target);
            w.AddComponent<FollowCameraComponent>(e, fc);
        }
    });

    // FollowCameraTag — empty marker, serialised as presence-only so the
    // tag survives save/load alongside FollowCameraComponent.
    reg.Register(std::type_index(typeid(FollowCameraTag)), {
        "FollowCameraTag",
        [](World& w, Entity e) { return w.GetComponent<FollowCameraTag>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            if (w.GetComponent<FollowCameraTag>(e))
                ss << "  FollowCameraTag:\n";
        },
        [](World& w, Entity e, const KVMap& /*kv*/, Resource::AssetManager*) {
            if (!w.HasComponent<FollowCameraTag>(e))
                w.AddComponent<FollowCameraTag>(e, FollowCameraTag{});
        }
    });

    // ==== AimCameraComponent (INLINE — third-person orbit VCam) ====
    reg.Register(std::type_index(typeid(AimCameraComponent)), {
        "AimCamera",
        [](World& w, Entity e) { return w.GetComponent<AimCameraComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ac = w.GetComponent<AimCameraComponent>(e);
            if (!ac) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  AimCamera: px=%.3f py=%.3f pz=%.3f"
                " yaw=%.4f pitch=%.4f dist=%.3f pMin=%.4f pMax=%.4f"
                " col=%d probe=%.3f",
                ac->pivotOffset.x, ac->pivotOffset.y, ac->pivotOffset.z,
                ac->yaw, ac->pitch, ac->distance, ac->pitchMin, ac->pitchMax,
                (int)ac->collisionAvoid, ac->probeRadius);
            ss << buf;
            WriteAttachmentRef(ss, "target", w, ac->target);
            ss << '\n';
            if (w.HasComponent<AimCameraTag>(e))
                ss << "  AimCameraTag:\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AimCameraComponent ac{};
            ac.pivotOffset    = { GetF(kv,"px",0.f), GetF(kv,"py",1.5f), GetF(kv,"pz",0.f) };
            ac.yaw            = GetF(kv,"yaw",       0.f);
            ac.pitch          = GetF(kv,"pitch",     0.f);
            ac.distance       = GetF(kv,"dist",      3.5f);
            ac.pitchMin       = GetF(kv,"pMin",     -1.4f);
            ac.pitchMax       = GetF(kv,"pMax",      1.0f);
            ac.collisionAvoid = GetI(kv,"col",       1) != 0;
            ac.probeRadius    = GetF(kv,"probe",     0.20f);
            ReadAttachmentRef(kv, "target", "target", w, e, ac.target);
            w.AddComponent<AimCameraComponent>(e, ac);
        }
    });

    reg.Register(std::type_index(typeid(AimCameraTag)), {
        "AimCameraTag",
        [](World& w, Entity e) { return w.GetComponent<AimCameraTag>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            if (w.GetComponent<AimCameraTag>(e))
                ss << "  AimCameraTag:\n";
        },
        [](World& w, Entity e, const KVMap& /*kv*/, Resource::AssetManager*) {
            if (!w.HasComponent<AimCameraTag>(e))
                w.AddComponent<AimCameraTag>(e, AimCameraTag{});
        }
    });

    // ==== SkyboxComponent (INLINE — paths are refs to .itex assets) ====
    reg.Register(std::type_index(typeid(SkyboxComponent)), {
        "Skybox",
        [](World& w, Entity e) { return w.GetComponent<SkyboxComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* s = w.GetComponent<SkyboxComponent>(e);
            if (!s) return;
            ss << "  Skybox: irradiance=" << PercentEncode(s->irradiancePath)
               << " radiance=" << PercentEncode(s->radiancePath)
               << " skybox=" << PercentEncode(s->skyboxPath)
               << " mips=" << s->radianceMipLevels
               << " strength=" << s->iblStrength << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            SkyboxComponent s;
            s.irradiancePath    = GetS(kv, "irradiance");
            s.radiancePath      = GetS(kv, "radiance");
            s.skyboxPath        = GetS(kv, "skybox");
            s.radianceMipLevels = static_cast<uint32_t>(GetI(kv, "mips", 7));
            s.iblStrength       = GetF(kv, "strength", 1.f);
            w.AddComponent<SkyboxComponent>(e, s);
        }
    });

    // ==== AtmosphereComponent (INLINE — atmospheric/IBL/aerial/star knobs) ====
    reg.Register(std::type_index(typeid(AtmosphereComponent)), {
        "Atmosphere",
        [](World& w, Entity e) { return w.GetComponent<AtmosphereComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* a = w.GetComponent<AtmosphereComponent>(e);
            if (!a) return;
            ss << "  Atmosphere: enabled=" << (a->atmosphereEnabled ? 1 : 0)
               << " source="  << static_cast<int>(a->skyboxSource)
               << " ibl=" << a->iblStrength
               << " aerial=" << (a->aerialCompositeEnabled ? 1 : 0)
               << " starDensity=" << a->starDensity
               << " starBright=" << a->starBrightness
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AtmosphereComponent a;
            a.atmosphereEnabled      = GetI(kv, "enabled", 1) != 0;
            a.skyboxSource           = static_cast<AtmosphereComponent::SkyboxSource>(
                                          GetI(kv, "source", 0));
            a.iblStrength            = GetF(kv, "ibl",      0.1f);
            a.aerialCompositeEnabled = GetI(kv, "aerial", 0) != 0;
            a.starDensity            = GetF(kv, "starDensity",  256.f);
            a.starBrightness         = GetF(kv, "starBright",   0.7f);
            w.AddComponent<AtmosphereComponent>(e, a);
        }
    });

    // ==== TODConfigComponent (INLINE — author Time-of-Day settings) ====
    reg.Register(std::type_index(typeid(TODConfigComponent)), {
        "TODConfig",
        [](World& w, Entity e) { return w.GetComponent<TODConfigComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<TODConfigComponent>(e);
            if (!c) return;
            ss << "  TODConfig: enabled=" << (c->enabled ? 1 : 0)
               << " t=" << c->timeOfDay
               << " speed=" << c->timeSpeed
               << " lat=" << c->latitudeRad
               << " sunScale=" << c->sunBrightnessScale
               << " moon=" << c->moonIntensityScale
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            TODConfigComponent c;
            c.enabled            = GetI(kv, "enabled", 0) != 0;
            c.timeOfDay          = GetF(kv, "t",     0.35f);
            c.timeSpeed          = GetF(kv, "speed", 0.f);
            c.latitudeRad        = GetF(kv, "lat",   0.6f);
            c.sunBrightnessScale = GetF(kv, "sunScale", 1.f);
            c.moonIntensityScale = GetF(kv, "moon",  0.015f);
            w.AddComponent<TODConfigComponent>(e, c);
        }
    });

    // ==== CloudComponent (INLINE — volumetric cloud tunables) ====
    reg.Register(std::type_index(typeid(CloudComponent)), {
        "Clouds",
        [](World& w, Entity e) { return w.GetComponent<CloudComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<CloudComponent>(e);
            if (!c) return;
            ss << "  Clouds: enabled=" << (c->enabled ? 1 : 0)
               << " bottom=" << c->bottomAltitude
               << " top=" << c->topAltitude
               << " coverage=" << c->coverage
               << " density=" << c->density
               << " noiseScale=" << c->noiseScale
               << " windDir=" << c->windDirection.x << "_" << c->windDirection.y << "_" << c->windDirection.z
               << " windSpeed=" << c->windSpeed
               << " aniso=" << c->anisotropy
               << " extinction=" << c->extinction
               << " ambient=" << c->ambientStrength
               << " color=" << c->cloudColor.x << "_" << c->cloudColor.y << "_" << c->cloudColor.z
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CloudComponent c;
            c.enabled         = GetI(kv, "enabled", 0) != 0;
            c.bottomAltitude  = GetF(kv, "bottom",     1500.f);
            c.topAltitude     = GetF(kv, "top",        4000.f);
            c.coverage        = GetF(kv, "coverage",   0.55f);
            c.density         = GetF(kv, "density",    1.0f);
            c.noiseScale      = GetF(kv, "noiseScale", 0.0008f);
            {
                auto cit = kv.find("windDir");
                if (cit != kv.end())
                    sscanf_s(cit->second.c_str(), "%f_%f_%f",
                             &c.windDirection.x, &c.windDirection.y, &c.windDirection.z);
            }
            c.windSpeed       = GetF(kv, "windSpeed",  8.f);
            c.anisotropy      = GetF(kv, "aniso",      0.55f);
            c.extinction      = GetF(kv, "extinction", 0.08f);
            c.ambientStrength = GetF(kv, "ambient",    0.35f);
            {
                auto cit = kv.find("color");
                if (cit != kv.end())
                    sscanf_s(cit->second.c_str(), "%f_%f_%f",
                             &c.cloudColor.x, &c.cloudColor.y, &c.cloudColor.z);
            }
            w.AddComponent<CloudComponent>(e, c);
        }
    });

    // ==== SunLightTag / MoonLightTag (TAGS — empty, presence-only) ====
    // Serialize as a single token; loader just adds the component.
    reg.Register(std::type_index(typeid(SunLightTag)), {
        "SunLight",
        [](World& w, Entity e) { return w.GetComponent<SunLightTag>(e) != nullptr; },
        [](World&, Entity, std::ostringstream& ss) { ss << "  SunLight: 1\n"; },
        [](World& w, Entity e, const KVMap&, Resource::AssetManager*) {
            w.AddComponent<SunLightTag>(e, SunLightTag{});
        }
    });
    reg.Register(std::type_index(typeid(MoonLightTag)), {
        "MoonLight",
        [](World& w, Entity e) { return w.GetComponent<MoonLightTag>(e) != nullptr; },
        [](World&, Entity, std::ostringstream& ss) { ss << "  MoonLight: 1\n"; },
        [](World& w, Entity e, const KVMap&, Resource::AssetManager*) {
            w.AddComponent<MoonLightTag>(e, MoonLightTag{});
        }
    });

    // ==== MaterialSourcePath (REF — stores path, ResourceSystem loads data) ====
    reg.Register(std::type_index(typeid(MaterialSourcePath)), {
        "MaterialRef",
        [](World& w, Entity e) { return w.GetComponent<MaterialSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* msp = w.GetComponent<MaterialSourcePath>(e);
            if (!msp || msp->path.empty()) return;
            ss << "  MaterialRef: path=" << PercentEncode(msp->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            std::string path = GetS(kv, "path");
            if (path.empty()) return;
            w.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ path });
            // Load the .imat into MaterialComponent
            MaterialComponent mat{};
            if (Resource::LoadMaterial(path, mat))
            {
                mat.SetDirty();
                if (auto* existing = w.GetComponent<MaterialComponent>(e))
                    *existing = mat;
                else
                    w.AddComponent<MaterialComponent>(e, std::move(mat));
            }
        }
    });

    // ==== MeshSourcePath (REF) ====
    // NOTE: the deserializer used to resurrect a SceneMeshHandle from a raw
    // .imsh path via AssetManager::AcquireMesh(). That entire legacy pool is
    // now gone (P1-P6 rewrite). The editor attaches MeshSourcePath purely as
    // a display hint; load-from-prefab does NOT auto-attach a render handle
    // — callers must re-spawn via the new MeshLibrary path (.meshlib +
    // meshId) to get visible geometry.
    reg.Register(std::type_index(typeid(MeshSourcePath)), {
        "MeshRef",
        [](World& w, Entity e) { return w.GetComponent<MeshSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* msp = w.GetComponent<MeshSourcePath>(e);
            if (!msp || msp->path.empty()) return;
            ss << "  MeshRef: path=" << PercentEncode(msp->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager* /*assetMgr*/) {
            std::string path = GetS(kv, "path");
            if (path.empty()) return;
            w.AddComponent<MeshSourcePath>(e, MeshSourcePath{ path });
        }
    });

    // ==== SceneSourcePath (REF — for skinned characters) ====
    reg.Register(std::type_index(typeid(SceneSourcePath)), {
        "SceneRef",
        [](World& w, Entity e) { return w.GetComponent<SceneSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ssp = w.GetComponent<SceneSourcePath>(e);
            if (!ssp || ssp->path.empty()) return;
            ss << "  SceneRef: path=" << PercentEncode(ssp->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            std::string path = GetS(kv, "path");
            if (!path.empty())
                w.AddComponent<SceneSourcePath>(e, SceneSourcePath{ path });
        }
    });

    // ==== AnimationSourcePath (REF) ====
    reg.Register(std::type_index(typeid(AnimationSourcePath)), {
        "AnimRef",
        [](World& w, Entity e) { return w.GetComponent<AnimationSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* asp = w.GetComponent<AnimationSourcePath>(e);
            if (!asp || asp->path.empty()) return;
            ss << "  AnimRef: path=" << PercentEncode(asp->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            std::string path = GetS(kv, "path");
            if (!path.empty())
                w.AddComponent<AnimationSourcePath>(e, AnimationSourcePath{ path });
        }
    });

    // ==== MaterialOverride (INLINE — property bag diff) ====
    reg.Register(std::type_index(typeid(MaterialOverride)), {
        "MaterialOverride",
        [](World& w, Entity e) { return w.GetComponent<MaterialOverride>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ovr = w.GetComponent<MaterialOverride>(e);
            if (!ovr || ovr->IsEmpty()) return;
            ss << "  MaterialOverride:";
            for (const auto& [key, val] : ovr->props)
            {
                ss << " " << PercentEncode(key) << "=";
                switch (val.type)
                {
                case MaterialOverride::Value::Float: ss << "f:" << val.data.f; break;
                case MaterialOverride::Value::Vec4:
                    ss << "v:" << val.data.v4[0] << "_" << val.data.v4[1]
                       << "_" << val.data.v4[2] << "_" << val.data.v4[3]; break;
                case MaterialOverride::Value::Int:   ss << "i:" << val.data.i; break;
                case MaterialOverride::Value::Bool:  ss << "b:" << (val.data.b ? 1 : 0); break;
                case MaterialOverride::Value::TexRef:ss << "t:" << PercentEncode(val.texPath); break;
                }
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            MaterialOverride ovr;
            for (const auto& [rawKey, rawVal] : kv)
            {
                auto colon = rawVal.find(':');
                if (colon == std::string::npos) continue;
                char typeChar = rawVal[0];
                std::string valStr = rawVal.substr(colon + 1);
                std::string key = PercentDecode(rawKey);
                try {
                    switch (typeChar) {
                    case 'f': ovr.Set(key, MaterialOverride::Value::MakeFloat(std::stof(valStr))); break;
                    case 'v': {
                        float x=0,y=0,z=0,ww=0;
                        sscanf_s(valStr.c_str(), "%f_%f_%f_%f", &x, &y, &z, &ww);
                        ovr.Set(key, MaterialOverride::Value::MakeVec4(x,y,z,ww)); } break;
                    case 'i': ovr.Set(key, MaterialOverride::Value::MakeInt(std::stoi(valStr))); break;
                    case 'b': ovr.Set(key, MaterialOverride::Value::MakeBool(valStr != "0")); break;
                    case 't': ovr.Set(key, MaterialOverride::Value::MakeTex(PercentDecode(valStr))); break;
                    }
                } catch (...) {}
            }
            if (!ovr.IsEmpty())
                w.AddComponent<MaterialOverride>(e, std::move(ovr));
        }
    });

    // ==== MeshHandle (INLINE — procedural primitives: 0=Cube, 1=Sphere, 2=Cone) ====
    reg.Register(std::type_index(typeid(MeshHandle)), {
        "MeshHandle",
        [](World& w, Entity e) { return w.GetComponent<MeshHandle>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* mh = w.GetComponent<MeshHandle>(e);
            if (!mh || !mh->IsValid()) return;
            char buf[64];
            snprintf(buf, sizeof(buf), "  MeshHandle: gpuMeshID=%u\n", mh->gpuMeshID);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            MeshHandle mh;
            mh.gpuMeshID = static_cast<uint32_t>(GetI(kv, "gpuMeshID", -1));
            if (mh.IsValid())
                w.AddComponent<MeshHandle>(e, mh);
        }
    });

    // ==== VisibilityComponent (INLINE) ====
    // Persist viewMask + flags. inheritedHidden is recomputed every frame by
    // TransformSystem from the parent chain, so it would only ever round-trip
    // a stale value — skip it.
    reg.Register(std::type_index(typeid(VisibilityComponent)), {
        "Visibility",
        [](World& w, Entity e) { return w.GetComponent<VisibilityComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* v = w.GetComponent<VisibilityComponent>(e);
            if (!v) return;
            char buf[96];
            snprintf(buf, sizeof(buf),
                "  Visibility: viewMask=%u flags=%u renderLayer=%u\n",
                v->viewMask,
                static_cast<uint32_t>(v->flags),
                static_cast<uint32_t>(v->renderLayer));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            VisibilityComponent v;
            v.viewMask    = GetU(kv, "viewMask", ViewBit::All);
            v.flags       = static_cast<uint8_t>(GetU(kv, "flags",
                                                       VisibilityComponent::Visible
                                                     | VisibilityComponent::CastShadow
                                                     | VisibilityComponent::RenderInMainPass));
            v.renderLayer = static_cast<uint8_t>(GetU(kv, "renderLayer", 0));
            w.AddComponent<VisibilityComponent>(e, v);
        }
    });

    // ==== BoundingVolume (INLINE — for mouse picking on primitives) ====
    reg.Register(std::type_index(typeid(BoundingVolume)), {
        "BoundingVolume",
        [](World& w, Entity e) { return w.GetComponent<BoundingVolume>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            ss << "  BoundingVolume:\n";
        },
        [](World& w, Entity e, const KVMap&, Resource::AssetManager*) {
            w.AddComponent<BoundingVolume>(e, BoundingVolume{});
        }
    });

    // ==== ChainPhysicsComponent (INLINE) ====
    reg.Register(std::type_index(typeid(ChainPhysicsComponent)), {
        "ChainPhysics",
        [](World& w, Entity e) { return w.GetComponent<ChainPhysicsComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* cp = w.GetComponent<ChainPhysicsComponent>(e);
            if (!cp) return;
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "  ChainPhysics: damping=%.4f gravity=%.4f stiffness=%.4f"
                " iterations=%d substeps=%d maxVelocity=%.4f localStiffness=%.4f"
                " skirtDamping=%.4f skirtGravity=%.4f skirtStiffness=%.4f"
                " skirtMaxVelocity=%.4f skirtLocalStiffness=%.4f skirtHorizDecay=%.4f"
                " springStiffness=%.4f springDamping=%.4f springMass=%.4f"
                " springGravity=%.4f springMaxDisp=%.4f"
                " springChildStiffness=%.4f springChildDamping=%.4f"
                " springChildMass=%.4f springChildGravity=%.4f springChildMaxDisp=%.4f\n",
                cp->damping, cp->gravity, cp->stiffness,
                cp->iterations, cp->substeps, cp->maxVelocity, cp->localStiffness,
                cp->skirtDamping, cp->skirtGravity, cp->skirtStiffness,
                cp->skirtMaxVelocity, cp->skirtLocalStiffness, cp->skirtHorizDecay,
                cp->springStiffness, cp->springDamping, cp->springMass,
                cp->springGravity, cp->springMaxDisp,
                cp->springChildStiffness, cp->springChildDamping,
                cp->springChildMass, cp->springChildGravity, cp->springChildMaxDisp);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ChainPhysicsComponent cp;
            cp.damping             = GetF(kv, "damping",             cp.damping);
            cp.gravity             = GetF(kv, "gravity",             cp.gravity);
            cp.stiffness           = GetF(kv, "stiffness",          cp.stiffness);
            cp.iterations          = GetI(kv, "iterations",          cp.iterations);
            cp.substeps            = GetI(kv, "substeps",            cp.substeps);
            cp.maxVelocity         = GetF(kv, "maxVelocity",        cp.maxVelocity);
            cp.localStiffness      = GetF(kv, "localStiffness",     cp.localStiffness);
            cp.skirtDamping        = GetF(kv, "skirtDamping",       cp.skirtDamping);
            cp.skirtGravity        = GetF(kv, "skirtGravity",       cp.skirtGravity);
            cp.skirtStiffness      = GetF(kv, "skirtStiffness",     cp.skirtStiffness);
            cp.skirtMaxVelocity    = GetF(kv, "skirtMaxVelocity",   cp.skirtMaxVelocity);
            cp.skirtLocalStiffness = GetF(kv, "skirtLocalStiffness",cp.skirtLocalStiffness);
            cp.skirtHorizDecay     = GetF(kv, "skirtHorizDecay",    cp.skirtHorizDecay);
            cp.springStiffness     = GetF(kv, "springStiffness",    cp.springStiffness);
            cp.springDamping       = GetF(kv, "springDamping",      cp.springDamping);
            cp.springMass          = GetF(kv, "springMass",          cp.springMass);
            cp.springGravity       = GetF(kv, "springGravity",      cp.springGravity);
            cp.springMaxDisp       = GetF(kv, "springMaxDisp",      cp.springMaxDisp);
            cp.springChildStiffness = GetF(kv, "springChildStiffness", cp.springChildStiffness);
            cp.springChildDamping   = GetF(kv, "springChildDamping",   cp.springChildDamping);
            cp.springChildMass      = GetF(kv, "springChildMass",      cp.springChildMass);
            cp.springChildGravity   = GetF(kv, "springChildGravity",   cp.springChildGravity);
            cp.springChildMaxDisp   = GetF(kv, "springChildMaxDisp",   cp.springChildMaxDisp);
            w.AddComponent<ChainPhysicsComponent>(e, cp);
        }
    });

    // ==== RigidBodyComponent (INLINE) ====
    reg.Register(std::type_index(typeid(RigidBodyComponent)), {
        "RigidBody",
        [](World& w, Entity e) { return w.GetComponent<RigidBodyComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* rb = w.GetComponent<RigidBodyComponent>(e);
            if (!rb) return;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "  RigidBody: motion=%u mass=%.4f linDamp=%.4f angDamp=%.4f"
                " friction=%.4f restitution=%.4f gravity=%.4f lockedAxes=%u\n",
                static_cast<uint32_t>(rb->motion),
                rb->mass, rb->linearDamping, rb->angularDamping,
                rb->friction, rb->restitution, rb->gravityFactor,
                static_cast<uint32_t>(rb->lockedAxes));
            ss << buf;
            // bodyId / lastBuilt* are runtime-only; PhysicsSystem lazily
            // creates the Jolt body on first sight, so we don't persist them.
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            RigidBodyComponent rb{};
            rb.motion         = static_cast<RigidBodyComponent::Motion>(
                                    GetI(kv, "motion", (int)RigidBodyComponent::Motion::Dynamic));
            rb.mass           = GetF(kv, "mass",        1.0f);
            rb.linearDamping  = GetF(kv, "linDamp",     0.05f);
            rb.angularDamping = GetF(kv, "angDamp",     0.05f);
            rb.friction       = GetF(kv, "friction",    0.5f);
            rb.restitution    = GetF(kv, "restitution", 0.0f);
            rb.gravityFactor  = GetF(kv, "gravity",     1.0f);
            // Missing-key default 0 keeps pre-axis-lock .iscene saves loading
            // with unconstrained 6-DOF, matching previous behavior.
            rb.lockedAxes     = static_cast<uint8_t>(GetU(kv, "lockedAxes", 0));
            rb.bodyId         = kInvalidPhysicsBodyId;
            w.AddComponent<RigidBodyComponent>(e, rb);
        }
    });

    // ==== ColliderComponent (INLINE) ====
    // Box / Sphere / Capsule fields are unconditionally written so prefab
    // editing in a text editor doesn't need to know the shape rules; the
    // deserializer reads each one with a sensible default if absent.
    reg.Register(std::type_index(typeid(ColliderComponent)), {
        "Collider",
        [](World& w, Entity e) { return w.GetComponent<ColliderComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<ColliderComponent>(e);
            if (!c) return;
            char buf[640];
            snprintf(buf, sizeof(buf),
                "  Collider: shape=%u"
                " halfExtents=%.4f_%.4f_%.4f radius=%.4f halfHeight=%.4f"
                " offset=%.4f_%.4f_%.4f"
                " rotEuler=%.4f_%.4f_%.4f"
                " meshPath=%s meshId=%u\n",
                static_cast<uint32_t>(c->shape),
                c->halfExtents.x, c->halfExtents.y, c->halfExtents.z,
                c->radius, c->halfHeight,
                c->offset.x, c->offset.y, c->offset.z,
                c->rotationEulerDeg.x, c->rotationEulerDeg.y, c->rotationEulerDeg.z,
                PercentEncode(c->meshLibPath).c_str(),
                c->meshLibMeshId);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ColliderComponent c{};
            c.shape         = static_cast<ColliderComponent::Shape>(
                                  GetI(kv, "shape", (int)ColliderComponent::Shape::Box));
            // halfExtents may arrive either as "x_y_z" or as 3 floats split by
            // the loader — match the LightData "color=" pattern (single key,
            // underscore-separated). Parse manually.
            {
                auto it = kv.find("halfExtents");
                if (it != kv.end()) {
                    float x = 0.5f, y = 0.5f, z = 0.5f;
                    if (sscanf_s(it->second.c_str(), "%f_%f_%f", &x, &y, &z) >= 3) {
                        c.halfExtents = { x, y, z };
                    }
                }
            }
            c.radius        = GetF(kv, "radius",     0.5f);
            c.halfHeight    = GetF(kv, "halfHeight", 0.5f);
            // Missing offset key in pre-existing saves → {0,0,0}, matching
            // the legacy "shape centered at entity pivot" behavior.
            {
                auto it = kv.find("offset");
                if (it != kv.end()) {
                    float x = 0.f, y = 0.f, z = 0.f;
                    if (sscanf_s(it->second.c_str(), "%f_%f_%f", &x, &y, &z) >= 3) {
                        c.offset = { x, y, z };
                    }
                }
            }
            // Same backward-compat pattern as offset — pre-rotation saves
            // omit the key, which falls through as identity rotation.
            {
                auto it = kv.find("rotEuler");
                if (it != kv.end()) {
                    float x = 0.f, y = 0.f, z = 0.f;
                    if (sscanf_s(it->second.c_str(), "%f_%f_%f", &x, &y, &z) >= 3) {
                        c.rotationEulerDeg = { x, y, z };
                    }
                }
            }
            c.meshLibPath   = GetS(kv, "meshPath",   "");
            c.meshLibMeshId = static_cast<uint32_t>(GetI(kv, "meshId", 0));
            w.AddComponent<ColliderComponent>(e, c);
        }
    });

    // ==== CharacterControllerComponent (INLINE) ====
    // Persists capsule + slope/gravity tuning. Runtime state (velocity,
    // isGrounded, groundEntity, timeInAir, bodyId, generation) is NOT
    // serialised — PhysicsSystem lazy-creates the JPH::CharacterVirtual on
    // first sight from the position/rotation already restored via
    // LocalTransform, so persisting runtime fields would only fight the
    // physics initialisation.
    reg.Register(std::type_index(typeid(CharacterControllerComponent)), {
        "CharacterController",
        [](World& w, Entity e) { return w.GetComponent<CharacterControllerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* cc = w.GetComponent<CharacterControllerComponent>(e);
            if (!cc) return;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "  CharacterController: radius=%.4f halfHeight=%.4f stepHeight=%.4f"
                " maxSlope=%.4f skinWidth=%.4f mass=%.3f pushStrength=%.3f"
                " gravity=%.3f maxFallSpeed=%.3f jumpSpeed=%.3f\n",
                cc->capsuleRadius, cc->capsuleHalfHeight, cc->stepHeight,
                cc->maxSlopeRad, cc->skinWidth, cc->mass, cc->pushStrength,
                cc->gravity, cc->maxFallSpeed, cc->jumpSpeed);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CharacterControllerComponent cc{};
            cc.capsuleRadius     = GetF(kv, "radius",       0.30f);
            cc.capsuleHalfHeight = GetF(kv, "halfHeight",   0.70f);
            cc.stepHeight        = GetF(kv, "stepHeight",   0.30f);
            cc.maxSlopeRad       = GetF(kv, "maxSlope",     0.7854f);
            cc.skinWidth         = GetF(kv, "skinWidth",    0.02f);
            cc.mass              = GetF(kv, "mass",         80.f);
            cc.pushStrength      = GetF(kv, "pushStrength", 1.0f);
            cc.gravity           = GetF(kv, "gravity",      -25.0f);
            cc.maxFallSpeed      = GetF(kv, "maxFallSpeed", 55.f);
            cc.jumpSpeed         = GetF(kv, "jumpSpeed",    7.5f);
            w.AddComponent<CharacterControllerComponent>(e, cc);
        }
    });

    // ==== PlayerComponent (INLINE) ====
    // Tuning + camera-basis ref. cameraEntity is an AttachmentRef now —
    // stored as camTargetGuid. Legacy "camEntity=<idx>" is honoured for
    // back-compat (remapped via SceneLoadContext at load time, then
    // immediately re-stamped as a GUID so the next save round-trips clean).
    reg.Register(std::type_index(typeid(PlayerComponent)), {
        "Player",
        [](World& w, Entity e) { return w.GetComponent<PlayerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* pc = w.GetComponent<PlayerComponent>(e);
            if (!pc) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  Player: walk=%.3f run=%.3f airControl=%.3f turnRate=%.3f"
                " facingMin=%.3f coyote=%.3f jumpBuffer=%.3f",
                pc->walkSpeed, pc->runSpeed, pc->airControl,
                pc->turnRate, pc->facingMinSpeed,
                pc->coyoteTime, pc->jumpBufferTime);
            ss << buf;
            WriteAttachmentRef(ss, "camTarget", w, pc->cameraEntity);
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            PlayerComponent pc{};
            pc.walkSpeed      = GetF(kv, "walk",        4.5f);
            pc.runSpeed       = GetF(kv, "run",         7.5f);
            pc.airControl     = GetF(kv, "airControl",  0.4f);
            pc.turnRate       = GetF(kv, "turnRate",   12.f);
            pc.facingMinSpeed = GetF(kv, "facingMin",   0.1f);
            pc.coyoteTime     = GetF(kv, "coyote",      0.10f);
            pc.jumpBufferTime = GetF(kv, "jumpBuffer",  0.10f);
            // New key is "camTargetGuid"; legacy key is "camEntity" (raw idx).
            ReadAttachmentRef(kv, "camTarget", "camEntity", w, e, pc.cameraEntity);
            w.AddComponent<PlayerComponent>(e, pc);
        }
    });

    // ==== AIComponent (INLINE) ====
    // Persists the tree path + tickInterval + enabled flag. The actual
    // BTAsset / BTInstance are runtime-only — AISystem::TickEntity lazily
    // re-resolves treePath → BTAsset via AcquireTree on first tick after
    // deserialisation.
    reg.Register(std::type_index(typeid(AIComponent)), {
        "AI",
        [](World& w, Entity e) { return w.GetComponent<AIComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ai = w.GetComponent<AIComponent>(e);
            if (!ai) return;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "  AI: enabled=%u tickInterval=%.4f treePath=%s\n",
                ai->enabled ? 1u : 0u,
                ai->tickInterval,
                PercentEncode(ai->treePath).c_str());
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AIComponent ai;
            ai.enabled       = GetI(kv, "enabled", 1) != 0;
            ai.tickInterval  = GetF(kv, "tickInterval", 0.1f);
            ai.treePath      = GetS(kv, "treePath", "");
            // tree / instance left null — AISystem::TickEntity hydrates them
            // the first time it sees the component with a non-empty path.
            w.AddComponent<AIComponent>(e, std::move(ai));
        }
    });

    // ==== BlackboardComponent (INLINE) ====
    // BB is runtime-only AI scratch state — chase timers, current wander
    // target, the spawn timestamp, animation-variant picks. Saving it
    // poisons every later play session with stale data from whatever
    // moment the prefab/world happened to be saved. AISystem lazy-creates
    // an empty BB on the first BT tick (AISystem.cpp:325), so we
    // intentionally disable both write AND read here:
    //   * shouldSerialize → always false → no new saves include BB
    //   * reader → no-op → old saves' "Blackboard:" tag is discarded
    // Re-enable only if you actually need persistent AI memory across
    // save/load (and then introduce a separate "PersistentBB" component
    // so transient keys still get wiped).
    reg.Register(std::type_index(typeid(BlackboardComponent)), {
        "Blackboard",
        [](World& /*w*/, Entity /*e*/) {
            return false;
        },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* bb = w.GetComponent<BlackboardComponent>(e);
            if (!bb || bb->values.empty()) return;
            ss << "  Blackboard: count=" << bb->values.size();
            int i = 0;
            for (const auto& [key, val] : bb->values)
            {
                char prefix[8];
                snprintf(prefix, sizeof(prefix), "k%d", i);
                ss << ' ' << prefix << '=' << PercentEncode(key);

                std::visit([&](auto&& v) {
                    using T = std::decay_t<decltype(v)>;
                    char buf[128];
                    if constexpr (std::is_same_v<T, bool>)
                        snprintf(buf, sizeof(buf), " t%d=B v%d=%u", i, i, v ? 1u : 0u);
                    else if constexpr (std::is_same_v<T, int>)
                        snprintf(buf, sizeof(buf), " t%d=I v%d=%d", i, i, v);
                    else if constexpr (std::is_same_v<T, float>)
                        snprintf(buf, sizeof(buf), " t%d=F v%d=%.6f", i, i, v);
                    else if constexpr (std::is_same_v<T, Entity>)
                        snprintf(buf, sizeof(buf), " t%d=E v%d=%u", i, i, static_cast<uint32_t>(v));
                    else if constexpr (std::is_same_v<T, DirectX::XMFLOAT3>)
                        snprintf(buf, sizeof(buf), " t%d=V v%d=%.6f_%.6f_%.6f",
                                  i, i, v.x, v.y, v.z);
                    else if constexpr (std::is_same_v<T, std::vector<DirectX::XMFLOAT3>>)
                    {
                        // Stream-emit since length is unbounded.
                        std::ostringstream tmp;
                        tmp << " t" << i << "=A v" << i << '=' << v.size();
                        for (const auto& p : v)
                            tmp << ':' << p.x << '_' << p.y << '_' << p.z;
                        std::snprintf(buf, sizeof(buf), "%s", tmp.str().c_str());
                    }
                    else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                    {
                        // String-list — each element percent-encoded so ':'
                        // and whitespace in the payload don't break the split.
                        std::ostringstream tmp;
                        tmp << " t" << i << "=SA v" << i << '=' << v.size();
                        for (const auto& s : v)
                            tmp << ':' << PercentEncode(s);
                        std::snprintf(buf, sizeof(buf), "%s", tmp.str().c_str());
                    }
                    else // string
                    {
                        // String — emit via stream so length isn't capped.
                        std::ostringstream tmp;
                        tmp << " t" << i << "=S v" << i << '=' << PercentEncode(v);
                        std::snprintf(buf, sizeof(buf), "%s", tmp.str().c_str());
                    }
                    ss << buf;
                }, val);
                ++i;
            }
            ss << '\n';
        },
        [](World& /*w*/, Entity /*e*/, const KVMap& /*kv*/, Resource::AssetManager*) {
            // Intentionally empty — BB is not loaded from saved files;
            // AISystem creates a fresh one on first BT tick.
            return;
        }
    });

#if 0 // legacy BB reader, kept for reference — see comment above the
      // current registration block. Re-enable + restore the original
      // shouldSerialize lambda if persistent BB is ever needed.
    reg.Register(std::type_index(typeid(BlackboardComponent)), {
        "Blackboard",
        [](World& w, Entity e) {
            const auto* bb = w.GetComponent<BlackboardComponent>(e);
            return bb && !bb->values.empty();
        },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* bb = w.GetComponent<BlackboardComponent>(e);
            if (!bb || bb->values.empty()) return;
            ss << "  Blackboard: count=" << bb->values.size();
            int i = 0;
            for (const auto& [key, val] : bb->values)
            {
                /* (writer body elided in #if 0) */
                (void)key; (void)val; (void)i;
            }
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            BlackboardComponent bb;
            const int count = GetI(kv, "count", 0);
            for (int i = 0; i < count; ++i)
            {
                char kkey[16], tkey[16], vkey[16];
                snprintf(kkey, sizeof(kkey), "k%d", i);
                snprintf(tkey, sizeof(tkey), "t%d", i);
                snprintf(vkey, sizeof(vkey), "v%d", i);
                const std::string name = GetS(kv, kkey, "");
                const std::string type = GetS(kv, tkey, "");
                const std::string val  = GetS(kv, vkey, "");
                if (name.empty() || type.empty()) continue;

                if      (type == "B") bb.values[name] = (val == "1" || val == "true");
                else if (type == "I") bb.values[name] = std::stoi(val.empty() ? "0" : val);
                else if (type == "F") bb.values[name] = std::stof(val.empty() ? "0" : val);
                else if (type == "E") bb.values[name] = static_cast<Entity>(
                                          std::stoul(val.empty() ? "0" : val));
                else if (type == "V")
                {
                    DirectX::XMFLOAT3 f3{};
                    sscanf_s(val.c_str(), "%f_%f_%f", &f3.x, &f3.y, &f3.z);
                    bb.values[name] = f3;
                }
                else if (type == "A")
                {
                    // val format: `n:x_y_z:x_y_z:...`. Split on ':' — first
                    // segment is the count, remaining are triples.
                    std::vector<DirectX::XMFLOAT3> arr;
                    size_t pos = val.find(':');
                    if (pos != std::string::npos)
                    {
                        const size_t expected = static_cast<size_t>(
                            std::stoul(val.substr(0, pos)));
                        arr.reserve(expected);
                        size_t cur = pos + 1;
                        while (cur < val.size())
                        {
                            const size_t next = val.find(':', cur);
                            const std::string triple = val.substr(
                                cur, next == std::string::npos
                                     ? std::string::npos : next - cur);
                            DirectX::XMFLOAT3 f3{};
                            sscanf_s(triple.c_str(), "%f_%f_%f",
                                     &f3.x, &f3.y, &f3.z);
                            arr.push_back(f3);
                            if (next == std::string::npos) break;
                            cur = next + 1;
                        }
                    }
                    bb.values[name] = std::move(arr);
                }
                else if (type == "SA")
                {
                    // val format: `n:enc1:enc2:...`. Mirror of the writer's
                    // SA branch above; each segment is percent-decoded.
                    std::vector<std::string> arr;
                    size_t pos = val.find(':');
                    if (pos != std::string::npos)
                    {
                        const size_t expected = static_cast<size_t>(
                            std::stoul(val.substr(0, pos)));
                        arr.reserve(expected);
                        size_t cur = pos + 1;
                        while (cur < val.size())
                        {
                            const size_t next = val.find(':', cur);
                            const std::string seg = val.substr(
                                cur, next == std::string::npos
                                     ? std::string::npos : next - cur);
                            arr.push_back(PercentDecode(seg));
                            if (next == std::string::npos) break;
                            cur = next + 1;
                        }
                    }
                    bb.values[name] = std::move(arr);
                }
                else if (type == "S") bb.values[name] = val;  // val came through PercentDecode in GetS
            }
            w.AddComponent<BlackboardComponent>(e, std::move(bb));
        }
    });
#endif // legacy BB serializer

    // ==== FootIKComponent (INLINE) ====
    // Chain indices are runtime-resolved on first tick (auto-detected from
    // PMX bone names) so we DON'T persist them — saved files would silently
    // break when the same character is re-imported with a different bone
    // ordering. Only the tunables travel across save/load.
    reg.Register(std::type_index(typeid(FootIKComponent)), {
        "FootIK",
        [](World& w, Entity e) { return w.GetComponent<FootIKComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* fk = w.GetComponent<FootIKComponent>(e);
            if (!fk) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  FootIK: enabled=%u weight=%.4f"
                " rayUp=%.4f rayDown=%.4f footOffset=%.4f"
                " alignNormal=%u pelvisDrop=%u\n",
                fk->enabled ? 1u : 0u, fk->enableWeight,
                fk->rayUp, fk->rayDown, fk->footOffset,
                fk->alignToNormal ? 1u : 0u, fk->pelvisDrop ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            FootIKComponent fk;
            fk.enabled       = GetI(kv, "enabled",     1) != 0;
            fk.enableWeight  = GetF(kv, "weight",      1.0f);
            fk.rayUp         = GetF(kv, "rayUp",       0.4f);
            fk.rayDown       = GetF(kv, "rayDown",     1.0f);
            fk.footOffset    = GetF(kv, "footOffset",  0.02f);
            fk.alignToNormal = GetI(kv, "alignNormal", 0) != 0;
            fk.pelvisDrop    = GetI(kv, "pelvisDrop",  0) != 0;
            // leftChainIdx / rightChainIdx left at -1 → auto-detect on load.
            w.AddComponent<FootIKComponent>(e, fk);
        }
    });

    // ==== AIIntentComponent (INLINE) ====
    // Strategic-layer intent — what the AI wants to achieve. Lifetime
    // is goal-scoped (seconds), independent of path/animation churn.
    reg.Register(std::type_index(typeid(AIIntentComponent)), {
        "AIIntent",
        [](World& w, Entity e) { return w.GetComponent<AIIntentComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* i = w.GetComponent<AIIntentComponent>(e);
            if (!i) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  AIIntent: goal=%u target=%u goalPos=%.4f_%.4f_%.4f"
                " priority=%.4f\n",
                static_cast<unsigned>(i->currentGoal),
                static_cast<unsigned>(i->targetEntity),
                i->goalPosition.x, i->goalPosition.y, i->goalPosition.z,
                i->goalPriority);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AIIntentComponent i;
            i.currentGoal  = static_cast<AIGoal>(GetI(kv, "goal", 0));
            i.targetEntity = static_cast<Entity>(GetI(kv, "target", 0));
            auto mit = kv.find("goalPos");
            if (mit != kv.end())
                sscanf_s(mit->second.c_str(), "%f_%f_%f",
                         &i.goalPosition.x, &i.goalPosition.y, &i.goalPosition.z);
            i.goalPriority = GetF(kv, "priority", 0.f);
            w.AddComponent<AIIntentComponent>(e, i);
        }
    });

    // ==== PerceptionComponent (INLINE) ====
    // Tunables only — the visibleEntities/heardSounds runtime tables are
    // re-populated by PerceptionSystem (not yet shipped) each tick.
    reg.Register(std::type_index(typeid(PerceptionComponent)), {
        "Perception",
        [](World& w, Entity e) { return w.GetComponent<PerceptionComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<PerceptionComponent>(e);
            if (!p) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  Perception: sightRange=%.4f sightConeDeg=%.4f"
                " hearingRange=%.4f audibleFor=%.4f\n",
                p->sightRange, p->sightConeDeg,
                p->hearingRange, p->audibleFor);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            PerceptionComponent p;
            p.sightRange   = GetF(kv, "sightRange",   15.f);
            p.sightConeDeg = GetF(kv, "sightConeDeg", 90.f);
            p.hearingRange = GetF(kv, "hearingRange", 12.f);
            p.audibleFor   = GetF(kv, "audibleFor",    8.f);
            w.AddComponent<PerceptionComponent>(e, p);
        }
    });

    // ==== NavAgentComponent (INLINE) ====
    // Tactical-layer intent + steering tunables. AITacticalSystem writes
    // destination/facingMode/lookTarget; persisted so a save snapshot the
    // mid-frame combat state. Runtime fields (path[], nextWaypoint,
    // pathValid, stuckTimer) are recomputed on the first tick after load.
    reg.Register(std::type_index(typeid(NavAgentComponent)), {
        "NavAgent",
        [](World& w, Entity e) { return w.GetComponent<NavAgentComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* a = w.GetComponent<NavAgentComponent>(e);
            if (!a) return;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "  NavAgent: hasDest=%u dest=%.4f_%.4f_%.4f"
                " useLookAt=%u lookTarget=%.4f_%.4f_%.4f"
                " facingMode=%u facingTarget=%u"
                " speed=%.4f arriveRadius=%.4f slowdownRadius=%.4f"
                " repathDistance=%.4f rotateToFacing=%u turnRate=%.4f\n",
                a->hasDestination ? 1u : 0u,
                a->destination.x, a->destination.y, a->destination.z,
                a->useLookAt ? 1u : 0u,
                a->lookTarget.x, a->lookTarget.y, a->lookTarget.z,
                static_cast<unsigned>(a->facingMode),
                static_cast<unsigned>(a->facingTarget),
                a->speed, a->arriveRadius, a->slowdownRadius,
                a->repathDistance,
                a->rotateToFacing ? 1u : 0u, a->turnRate);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            NavAgentComponent a;
            a.hasDestination = GetI(kv, "hasDest", 0) != 0;
            auto dit = kv.find("dest");
            if (dit != kv.end())
                sscanf_s(dit->second.c_str(), "%f_%f_%f",
                         &a.destination.x, &a.destination.y, &a.destination.z);
            a.useLookAt = GetI(kv, "useLookAt", 0) != 0;
            auto lit = kv.find("lookTarget");
            if (lit != kv.end())
                sscanf_s(lit->second.c_str(), "%f_%f_%f",
                         &a.lookTarget.x, &a.lookTarget.y, &a.lookTarget.z);
            a.facingMode    = static_cast<NavFacingMode>(GetI(kv, "facingMode", 0));
            a.facingTarget  = static_cast<Entity>(GetI(kv, "facingTarget", 0));
            a.speed          = GetF(kv, "speed",          3.0f);
            a.arriveRadius   = GetF(kv, "arriveRadius",   0.25f);
            a.slowdownRadius = GetF(kv, "slowdownRadius", 1.5f);
            a.repathDistance = GetF(kv, "repathDistance", 1.0f);
            a.rotateToFacing = GetI(kv, "rotateToFacing", 1) != 0;
            a.turnRate       = GetF(kv, "turnRate",       8.0f);
            // Pre-refactor saves carried hasTarget/target/groundSnap/
            // groundSnapHeight on this line — silently ignored. Move
            // destination now lives on this same component under
            // dest/hasDest, ground snap belongs to CCC.
            w.AddComponent<NavAgentComponent>(e, a);
        }
    });

    // ==== CapsuleColliderComponent (INLINE) ====
    reg.Register(std::type_index(typeid(CapsuleColliderComponent)), {
        "CapsuleCollider",
        [](World& w, Entity e) { return w.GetComponent<CapsuleColliderComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* cc = w.GetComponent<CapsuleColliderComponent>(e);
            if (!cc || cc->count == 0) return;
            // Write each capsule as a separate key-value line.
            ss << "  CapsuleCollider: count=" << cc->count;
            for (int i = 0; i < cc->count; ++i)
            {
                const auto& c = cc->capsules[i];
                char buf[256];
                snprintf(buf, sizeof(buf),
                    " c%d_boneA=%u c%d_boneB=%u c%d_radius=%.4f c%d_enabled=%d"
                    " c%d_offAx=%.4f c%d_offAy=%.4f c%d_offAz=%.4f"
                    " c%d_offBx=%.4f c%d_offBy=%.4f c%d_offBz=%.4f",
                    i, c.boneA, i, c.boneB, i, c.radius, i, c.enabled ? 1 : 0,
                    i, c.offsetA.x, i, c.offsetA.y, i, c.offsetA.z,
                    i, c.offsetB.x, i, c.offsetB.y, i, c.offsetB.z);
                ss << buf;
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CapsuleColliderComponent cc;
            cc.count = GetI(kv, "count", 0);
            if (cc.count > CapsuleColliderComponent::MAX_CAPSULES)
                cc.count = CapsuleColliderComponent::MAX_CAPSULES;
            for (int i = 0; i < cc.count; ++i)
            {
                auto& c = cc.capsules[i];
                char key[32];
                snprintf(key, sizeof(key), "c%d_boneA", i);   c.boneA   = static_cast<uint32_t>(GetI(kv, key, 0));
                snprintf(key, sizeof(key), "c%d_boneB", i);   c.boneB   = static_cast<uint32_t>(GetI(kv, key, 0));
                snprintf(key, sizeof(key), "c%d_radius", i);  c.radius  = GetF(kv, key, 0.05f);
                snprintf(key, sizeof(key), "c%d_enabled", i); c.enabled = GetI(kv, key, 1) != 0;
                snprintf(key, sizeof(key), "c%d_offAx", i);   c.offsetA.x = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offAy", i);   c.offsetA.y = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offAz", i);   c.offsetA.z = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offBx", i);   c.offsetB.x = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offBy", i);   c.offsetB.y = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offBz", i);   c.offsetB.z = GetF(kv, key, 0.f);
            }
            w.AddComponent<CapsuleColliderComponent>(e, cc);
        }
    });

    // ==== ScriptComponent (INLINE — one "Script:" line per attached slot) ====
    // An entity may have several scripts. We emit one indented "Script:" line
    // per slot; on load the scene/prefab dispatcher calls our deserialize once
    // per line (see SceneSerializer dispatch loop), so each line APPENDS a slot
    // to the entity's ScriptComponent. Old single-line saves load unchanged as
    // a one-slot component. An empty component emits a "slots=0" marker line so
    // it survives a round-trip.
    reg.Register(std::type_index(typeid(ScriptComponent)), {
        "Script",
        [](World& w, Entity e) { return w.GetComponent<ScriptComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* sc = w.GetComponent<ScriptComponent>(e);
            if (!sc) return;
            if (sc->scripts.empty())
            {
                ss << "  Script: slots=0\n";  // marker — keep the (inert) component on reload
                return;
            }
            for (const auto& si : sc->scripts)
            {
                ss << "  Script: path=" << PercentEncode(si.scriptPath)
                   << " enabled=" << (si.enabled ? 1 : 0);
                // Exposed-variable overrides — one `var.<name>=<typechar>:<val>`
                // token per entry (mirrors the MaterialOverride inline format).
                for (const auto& [name, val] : si.vars)
                {
                    ss << " var." << PercentEncode(name) << "=" << ScriptVarTypeToChar(val.type) << ":";
                    switch (val.type)
                    {
                    case ScriptVarType::Float:  ss << val.data.f; break;
                    case ScriptVarType::Int:    ss << val.data.i; break;
                    case ScriptVarType::Bool:   ss << (val.data.b ? 1 : 0); break;
                    case ScriptVarType::Float3:
                    case ScriptVarType::Color:  ss << val.data.v3[0] << "_" << val.data.v3[1]
                                                   << "_" << val.data.v3[2]; break;
                    case ScriptVarType::Entity: ss << val.data.entity; break;
                    case ScriptVarType::String:
                    case ScriptVarType::Asset:  ss << PercentEncode(val.str); break;
                    }
                }
                ss << "\n";
            }
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            // Get-or-create the component, then append this line's slot. Calling
            // AddComponent fresh would clobber slots appended by earlier lines.
            ScriptComponent* sc = w.GetComponent<ScriptComponent>(e);
            if (!sc) { w.AddComponent<ScriptComponent>(e, {}); sc = w.GetComponent<ScriptComponent>(e); }
            if (!sc) return;

            // Marker line for an empty component (no "path" key) → no slot.
            auto pathIt = kv.find("path");
            if (pathIt == kv.end()) return;

            ScriptInstance si;
            si.scriptPath = PercentDecode(pathIt->second);
            si.enabled    = GetI(kv, "enabled", 1) != 0;
            // Parse exposed-variable override tokens (`var.<name>=<typechar>:<val>`).
            // Absent in old scenes → empty bag (script defaults apply).
            for (const auto& [rawKey, rawVal] : kv)
            {
                if (rawKey.rfind("var.", 0) != 0) continue;
                if (rawVal.size() < 2 || rawVal[1] != ':') continue;
                ScriptVarType type;
                if (!ScriptVarTypeFromChar(rawVal[0], type)) continue;
                const std::string name = PercentDecode(rawKey.substr(4));
                const std::string vs   = rawVal.substr(2);
                try {
                    switch (type)
                    {
                    case ScriptVarType::Float:  si.vars[name] = ScriptVarValue::MakeFloat(std::stof(vs)); break;
                    case ScriptVarType::Int:    si.vars[name] = ScriptVarValue::MakeInt(std::stoi(vs));   break;
                    case ScriptVarType::Bool:   si.vars[name] = ScriptVarValue::MakeBool(vs != "0");      break;
                    case ScriptVarType::Float3:
                    case ScriptVarType::Color:
                    {
                        float x = 0.f, y = 0.f, z = 0.f;
                        sscanf_s(vs.c_str(), "%f_%f_%f", &x, &y, &z);
                        si.vars[name] = (type == ScriptVarType::Color)
                            ? ScriptVarValue::MakeColor (x, y, z)
                            : ScriptVarValue::MakeFloat3(x, y, z);
                    } break;
                    case ScriptVarType::Entity: si.vars[name] = ScriptVarValue::MakeEntity((uint32_t)std::stoul(vs)); break;
                    case ScriptVarType::String: si.vars[name] = ScriptVarValue::MakeString(PercentDecode(vs)); break;
                    case ScriptVarType::Asset:  si.vars[name] = ScriptVarValue::MakeAsset (PercentDecode(vs)); break;
                    }
                } catch (...) {}
            }
            sc->scripts.push_back(std::move(si));
        }
    });

    // ==== SocketComponent (INLINE — per-entity attachment points) ====
    // Layout in serialized form:
    //   Sockets: count=N n0=<percent-encoded-name> b0=<boneIdx> m0=<f0_..._f15>
    //                    n1=...                   b1=...        m1=...
    // worldTransform is intentionally NOT serialized — SocketSystem rewrites it
    // every frame from the bone pose, so any saved value would be stale.
    reg.Register(std::type_index(typeid(SocketComponent)), {
        "Sockets",
        [](World& w, Entity e) {
            const auto* sc = w.GetComponent<SocketComponent>(e);
            return sc && sc->count > 0;
        },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* sc = w.GetComponent<SocketComponent>(e);
            if (!sc || sc->count == 0) return;
            ss << "  Sockets: count=" << sc->count;
            char buf[32];
            for (uint32_t i = 0; i < sc->count; ++i)
            {
                const auto& s = sc->sockets[i];
                ss << " n" << i << "=" << PercentEncode(s.name);
                ss << " b" << i << "=" << s.boneIndex;
                ss << " m" << i << "=";
                const float* p = reinterpret_cast<const float*>(&s.localOffset);
                for (int k = 0; k < 16; ++k)
                {
                    if (k > 0) ss << "_";
                    snprintf(buf, sizeof(buf), "%.6f", p[k]);
                    ss << buf;
                }
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            SocketComponent sc;
            int count = GetI(kv, "count", 0);
            if (count < 0) count = 0;
            if (count > static_cast<int>(SocketComponent::MAX))
                count = static_cast<int>(SocketComponent::MAX);
            sc.count = static_cast<uint32_t>(count);

            for (uint32_t i = 0; i < sc.count; ++i)
            {
                const std::string nKey = "n" + std::to_string(i);
                const std::string bKey = "b" + std::to_string(i);
                const std::string mKey = "m" + std::to_string(i);

                auto& s = sc.sockets[i];
                const std::string name = PercentDecode(GetS(kv, nKey.c_str(), ""));
                const size_t copy = std::min(name.size(), sizeof(s.name) - 1);
                std::memcpy(s.name, name.c_str(), copy);
                s.name[copy] = '\0';
                s.boneIndex = static_cast<uint32_t>(GetI(kv, bKey.c_str(), 0));

                DirectX::XMStoreFloat4x4(&s.localOffset, DirectX::XMMatrixIdentity());
                DirectX::XMStoreFloat4x4(&s.worldTransform, DirectX::XMMatrixIdentity());

                const std::string mStr = GetS(kv, mKey.c_str(), "");
                if (!mStr.empty())
                {
                    float* p = reinterpret_cast<float*>(&s.localOffset);
                    size_t start = 0;
                    for (int k = 0; k < 16; ++k)
                    {
                        size_t end = mStr.find('_', start);
                        if (end == std::string::npos) end = mStr.size();
                        try { p[k] = std::stof(mStr.substr(start, end - start)); }
                        catch (...) {}
                        if (end == mStr.size()) break;
                        start = end + 1;
                    }

                    // Seed Euler authoring repr from the loaded matrix so the
                    // inspector starts on a stable representation. Lossy near
                    // gimbal lock — fine for socket use cases.
                    using namespace DirectX;
                    XMVECTOR vScl, vRot, vTrn;
                    if (XMMatrixDecompose(&vScl, &vRot, &vTrn,
                                          XMLoadFloat4x4(&s.localOffset)))
                    {
                        XMFLOAT4 q;
                        XMStoreFloat4(&q, vRot);
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
                        s.rotationEulerDeg = {
                            XMConvertToDegrees(rx),
                            XMConvertToDegrees(ry),
                            XMConvertToDegrees(rz) };
                    }
                }
            }

            if (auto* existing = w.GetComponent<SocketComponent>(e))
                *existing = sc;
            else
                w.AddComponent<SocketComponent>(e, sc);
        }
    });

    // ---- UI components ------------------------------------------------------
    // Texture/widget runtime state (UIRootComponent::root, UIImageComponent::
    // srvGpuHandle, WorldUIImageComponent::bindlessIndex, UIFocusComponent::
    // focusedWidget) is intentionally NOT serialized — those are populated by
    // gameplay/Lua/TextureSystem after spawn. UIFocusComponent and
    // DamageNumberComponent are runtime-only and have no serializers.

    // ==== UI::UIRootComponent (INLINE — widget tree skipped) ====
    reg.Register(std::type_index(typeid(UI::UIRootComponent)), {
        "UIRoot",
        [](World& w, Entity e) { return w.GetComponent<UI::UIRootComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* r = w.GetComponent<UI::UIRootComponent>(e);
            if (!r) return;
            ss << "  UIRoot: name=" << PercentEncode(r->name)
               << " sortOrder=" << r->sortOrder
               << " visible=" << (r->visible ? 1u : 0u)
               << " inputEnabled=" << (r->inputEnabled ? 1u : 0u)
               << " canvasW=" << r->canvasSizeOverride.x
               << " canvasH=" << r->canvasSizeOverride.y << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UIRootComponent r;
            r.name               = GetS(kv, "name");
            r.sortOrder          = GetI(kv, "sortOrder", 0);
            r.visible            = GetI(kv, "visible", 1) != 0;
            r.inputEnabled       = GetI(kv, "inputEnabled", 1) != 0;
            r.canvasSizeOverride = { GetF(kv, "canvasW", 0.f), GetF(kv, "canvasH", 0.f) };
            // r.root left null — gameplay code repopulates the widget tree.
            w.AddComponent<UI::UIRootComponent>(e, std::move(r));
        }
    });

    // ==== UI::UIScreenSpaceComponent (INLINE) ====
    reg.Register(std::type_index(typeid(UI::UIScreenSpaceComponent)), {
        "UIScreenSpace",
        [](World& w, Entity e) { return w.GetComponent<UI::UIScreenSpaceComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* s = w.GetComponent<UI::UIScreenSpaceComponent>(e);
            if (!s) return;
            char buf[160];
            snprintf(buf, sizeof(buf),
                "  UIScreenSpace: anchorX=%.4f anchorY=%.4f offsetX=%.4f offsetY=%.4f"
                " pivotX=%.4f pivotY=%.4f\n",
                s->anchorX, s->anchorY, s->offsetX, s->offsetY, s->pivotX, s->pivotY);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UIScreenSpaceComponent s;
            s.anchorX = GetF(kv, "anchorX", 0.f);
            s.anchorY = GetF(kv, "anchorY", 0.f);
            s.offsetX = GetF(kv, "offsetX", 0.f);
            s.offsetY = GetF(kv, "offsetY", 0.f);
            s.pivotX  = GetF(kv, "pivotX",  0.f);
            s.pivotY  = GetF(kv, "pivotY",  0.f);
            w.AddComponent<UI::UIScreenSpaceComponent>(e, s);
        }
    });

    // ==== UI::UIImageComponent (INLINE — srvGpuHandle is runtime) ====
    reg.Register(std::type_index(typeid(UI::UIImageComponent)), {
        "UIImage",
        [](World& w, Entity e) { return w.GetComponent<UI::UIImageComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* i = w.GetComponent<UI::UIImageComponent>(e);
            if (!i) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  UIImage: sizeX=%.4f sizeY=%.4f uv0X=%.4f uv0Y=%.4f"
                " uv1X=%.4f uv1Y=%.4f tint=%.4f_%.4f_%.4f_%.4f visible=%u\n",
                i->sizeX, i->sizeY, i->uv0X, i->uv0Y, i->uv1X, i->uv1Y,
                i->tint.x, i->tint.y, i->tint.z, i->tint.w,
                i->visible ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UIImageComponent i;
            i.sizeX = GetF(kv, "sizeX", 64.f);
            i.sizeY = GetF(kv, "sizeY", 64.f);
            i.uv0X  = GetF(kv, "uv0X", 0.f);
            i.uv0Y  = GetF(kv, "uv0Y", 0.f);
            i.uv1X  = GetF(kv, "uv1X", 1.f);
            i.uv1Y  = GetF(kv, "uv1Y", 1.f);
            const std::string ts = GetS(kv, "tint");
            if (!ts.empty())
                sscanf_s(ts.c_str(), "%f_%f_%f_%f",
                    &i.tint.x, &i.tint.y, &i.tint.z, &i.tint.w);
            i.visible = GetI(kv, "visible", 1) != 0;
            // srvGpuHandle stays 0 — repopulate via TextureSystem after spawn.
            w.AddComponent<UI::UIImageComponent>(e, i);
        }
    });

    // ==== UI::UITextComponent (INLINE) ====
    reg.Register(std::type_index(typeid(UI::UITextComponent)), {
        "UIText",
        [](World& w, Entity e) { return w.GetComponent<UI::UITextComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* t = w.GetComponent<UI::UITextComponent>(e);
            if (!t) return;
            ss << "  UIText: text=" << PercentEncode(t->text)
               << " color=" << t->color.x << "_" << t->color.y << "_"
               << t->color.z << "_" << t->color.w
               << " scale=" << t->scale
               << " visible=" << (t->visible ? 1u : 0u) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UITextComponent t;
            t.text = GetS(kv, "text");
            const std::string cs = GetS(kv, "color");
            if (!cs.empty())
                sscanf_s(cs.c_str(), "%f_%f_%f_%f",
                    &t.color.x, &t.color.y, &t.color.z, &t.color.w);
            t.scale   = GetF(kv, "scale", 1.f);
            t.visible = GetI(kv, "visible", 1) != 0;
            w.AddComponent<UI::UITextComponent>(e, t);
        }
    });

    // ==== UI::UIBarComponent (INLINE) ====
    reg.Register(std::type_index(typeid(UI::UIBarComponent)), {
        "UIBar",
        [](World& w, Entity e) { return w.GetComponent<UI::UIBarComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* b = w.GetComponent<UI::UIBarComponent>(e);
            if (!b) return;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "  UIBar: value=%.4f sizeX=%.4f sizeY=%.4f borderThick=%.4f"
                " fill=%.4f_%.4f_%.4f_%.4f bg=%.4f_%.4f_%.4f_%.4f"
                " border=%.4f_%.4f_%.4f_%.4f visible=%u\n",
                b->value, b->sizeX, b->sizeY, b->borderThick,
                b->fillColor.x, b->fillColor.y, b->fillColor.z, b->fillColor.w,
                b->backgroundColor.x, b->backgroundColor.y, b->backgroundColor.z, b->backgroundColor.w,
                b->borderColor.x, b->borderColor.y, b->borderColor.z, b->borderColor.w,
                b->visible ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UIBarComponent b;
            b.value       = GetF(kv, "value", 1.f);
            b.sizeX       = GetF(kv, "sizeX", 200.f);
            b.sizeY       = GetF(kv, "sizeY", 24.f);
            b.borderThick = GetF(kv, "borderThick", 1.f);
            auto parseV4 = [&](const char* k, DirectX::XMFLOAT4& out) {
                const std::string s = GetS(kv, k);
                if (!s.empty()) sscanf_s(s.c_str(), "%f_%f_%f_%f", &out.x, &out.y, &out.z, &out.w);
            };
            parseV4("fill",   b.fillColor);
            parseV4("bg",     b.backgroundColor);
            parseV4("border", b.borderColor);
            b.visible = GetI(kv, "visible", 1) != 0;
            w.AddComponent<UI::UIBarComponent>(e, b);
        }
    });

    // ==== UI::WorldSpaceUIComponent (INLINE — computed fields skipped) ====
    reg.Register(std::type_index(typeid(UI::WorldSpaceUIComponent)), {
        "WorldUI",
        [](World& w, Entity e) { return w.GetComponent<UI::WorldSpaceUIComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* u = w.GetComponent<UI::WorldSpaceUIComponent>(e);
            if (!u) return;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "  WorldUI: baseW=%.4f baseH=%.4f pivotX=%.4f pivotY=%.4f"
                " offX=%.4f offY=%.4f offZ=%.4f scalingMode=%u"
                " fadeNear=%.4f fadeFar=%.4f hideBehind=%u depthMode=%u\n",
                u->baseSize.x, u->baseSize.y, u->pivot.x, u->pivot.y,
                u->screenSpaceOffset.x, u->screenSpaceOffset.y, u->screenSpaceOffset.z,
                static_cast<uint32_t>(u->scalingMode),
                u->fadeNear, u->fadeFar,
                u->hideWhenBehindCamera ? 1u : 0u,
                static_cast<uint32_t>(u->depthMode));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::WorldSpaceUIComponent u;
            u.baseSize          = { GetF(kv, "baseW", 1.f),  GetF(kv, "baseH", 0.2f) };
            u.pivot             = { GetF(kv, "pivotX", 0.5f), GetF(kv, "pivotY", 1.f) };
            u.screenSpaceOffset = { GetF(kv, "offX", 0.f), GetF(kv, "offY", 0.f), GetF(kv, "offZ", 0.f) };
            u.scalingMode       = static_cast<UI::ScalingMode>(GetI(kv, "scalingMode", (int)UI::ScalingMode::ConstantWorld));
            u.fadeNear          = GetF(kv, "fadeNear", 0.f);
            u.fadeFar           = GetF(kv, "fadeFar", 50.f);
            u.hideWhenBehindCamera = GetI(kv, "hideBehind", 1) != 0;
            u.depthMode         = static_cast<UI::DepthMode>(GetI(kv, "depthMode", (int)UI::DepthMode::Always));
            w.AddComponent<UI::WorldSpaceUIComponent>(e, u);
        }
    });

    // ==== UI::WorldUIBarComponent (INLINE) ====
    reg.Register(std::type_index(typeid(UI::WorldUIBarComponent)), {
        "WorldUIBar",
        [](World& w, Entity e) { return w.GetComponent<UI::WorldUIBarComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* b = w.GetComponent<UI::WorldUIBarComponent>(e);
            if (!b) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  WorldUIBar: value=%.4f borderThick=%.4f"
                " fill=%.4f_%.4f_%.4f_%.4f bg=%.4f_%.4f_%.4f_%.4f"
                " border=%.4f_%.4f_%.4f_%.4f visible=%u\n",
                b->value, b->borderThick,
                b->fillColor.x, b->fillColor.y, b->fillColor.z, b->fillColor.w,
                b->backgroundColor.x, b->backgroundColor.y, b->backgroundColor.z, b->backgroundColor.w,
                b->borderColor.x, b->borderColor.y, b->borderColor.z, b->borderColor.w,
                b->visible ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::WorldUIBarComponent b;
            b.value       = GetF(kv, "value", 1.f);
            b.borderThick = GetF(kv, "borderThick", 0.02f);
            auto parseV4 = [&](const char* k, DirectX::XMFLOAT4& out) {
                const std::string s = GetS(kv, k);
                if (!s.empty()) sscanf_s(s.c_str(), "%f_%f_%f_%f", &out.x, &out.y, &out.z, &out.w);
            };
            parseV4("fill",   b.fillColor);
            parseV4("bg",     b.backgroundColor);
            parseV4("border", b.borderColor);
            b.visible = GetI(kv, "visible", 1) != 0;
            w.AddComponent<UI::WorldUIBarComponent>(e, b);
        }
    });

    // ==== UI::WorldUITextComponent (INLINE) ====
    reg.Register(std::type_index(typeid(UI::WorldUITextComponent)), {
        "WorldUIText",
        [](World& w, Entity e) { return w.GetComponent<UI::WorldUITextComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* t = w.GetComponent<UI::WorldUITextComponent>(e);
            if (!t) return;
            ss << "  WorldUIText: text=" << PercentEncode(t->text)
               << " color=" << t->color.x << "_" << t->color.y << "_"
               << t->color.z << "_" << t->color.w
               << " scale=" << t->scale
               << " visible=" << (t->visible ? 1u : 0u) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::WorldUITextComponent t;
            t.text = GetS(kv, "text");
            const std::string cs = GetS(kv, "color");
            if (!cs.empty())
                sscanf_s(cs.c_str(), "%f_%f_%f_%f",
                    &t.color.x, &t.color.y, &t.color.z, &t.color.w);
            t.scale   = GetF(kv, "scale", 1.f);
            t.visible = GetI(kv, "visible", 1) != 0;
            w.AddComponent<UI::WorldUITextComponent>(e, t);
        }
    });

    // ==== UI::WorldUIImageComponent (INLINE — bindlessIndex is runtime) ====
    reg.Register(std::type_index(typeid(UI::WorldUIImageComponent)), {
        "WorldUIImage",
        [](World& w, Entity e) { return w.GetComponent<UI::WorldUIImageComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* i = w.GetComponent<UI::WorldUIImageComponent>(e);
            if (!i) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  WorldUIImage: tint=%.4f_%.4f_%.4f_%.4f"
                " uv0X=%.4f uv0Y=%.4f uv1X=%.4f uv1Y=%.4f visible=%u\n",
                i->tint.x, i->tint.y, i->tint.z, i->tint.w,
                i->uv0.x, i->uv0.y, i->uv1.x, i->uv1.y,
                i->visible ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::WorldUIImageComponent i;
            const std::string ts = GetS(kv, "tint");
            if (!ts.empty())
                sscanf_s(ts.c_str(), "%f_%f_%f_%f",
                    &i.tint.x, &i.tint.y, &i.tint.z, &i.tint.w);
            i.uv0 = { GetF(kv, "uv0X", 0.f), GetF(kv, "uv0Y", 0.f) };
            i.uv1 = { GetF(kv, "uv1X", 1.f), GetF(kv, "uv1Y", 1.f) };
            i.visible = GetI(kv, "visible", 1) != 0;
            // bindlessIndex stays ~0u — repopulate via TextureSystem after spawn.
            w.AddComponent<UI::WorldUIImageComponent>(e, i);
        }
    });
}

ComponentSerializerRegistry& GetComponentRegistry()
{
    static ComponentSerializerRegistry s_reg;
    static bool s_init = false;
    if (!s_init)
    {
        RegisterAllComponentSerializers(s_reg);
        s_init = true;
    }
    return s_reg;
}
