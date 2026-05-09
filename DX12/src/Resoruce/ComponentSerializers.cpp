#include "Resource/ComponentSerializers.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/DDGIComponents.h"
#include "ECS/VolumeComponent.h"
#include "UI/UIComponents.h"
#include "UI/WorldSpaceUI.h"
#include "Physics/ChainPhysicsSystem.h"
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
    // (Renderer reassigns on world load) and BAKED is dropped because the
    // capture pass re-runs after load anyway.
    reg.Register(std::type_index(typeid(ReflectionProbeComponent)), {
        "ReflectionProbe",
        [](World& w, Entity e) { return w.GetComponent<ReflectionProbeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* rp = w.GetComponent<ReflectionProbeComponent>(e);
            if (!rp) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  ReflectionProbe: innerExtents=%.4f,%.4f,%.4f outerExtents=%.4f,%.4f,%.4f"
                " realtime=%u tickIntervalFrames=%u\n",
                rp->innerExtents.x, rp->innerExtents.y, rp->innerExtents.z,
                rp->outerExtents.x, rp->outerExtents.y, rp->outerExtents.z,
                rp->realtime ? 1u : 0u, rp->tickIntervalFrames);
            ss << buf;
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
                " reloc=%u classify=%u prio=%d cascade=%u dbg=%u"
                " tx=%.4f ty=%.4f tz=%.4f tscale=%.4f\n",
                v->origin.x, v->origin.y, v->origin.z,
                v->extent.x, v->extent.y, v->extent.z,
                v->probeCountsX, v->probeCountsY, v->probeCountsZ, v->raysPerProbe,
                v->hysteresis, v->normalBias, v->viewBias, v->boundaryFadeRatio,
                v->enableRelocation     ? 1u : 0u,
                v->enableClassification ? 1u : 0u,
                v->priority, v->cascadeLevel,
                v->debugDraw            ? 1u : 0u,
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
            v.debugDraw     = GetI(kv, "dbg",     v.debugDraw ? 1 : 0) != 0;
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

    // ==== CameraComponent (INLINE) ====
    reg.Register(std::type_index(typeid(CameraComponent)), {
        "Camera",
        [](World& w, Entity e) { return w.GetComponent<CameraComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<CameraComponent>(e);
            if (!c) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  Camera: px=%.4f py=%.4f pz=%.4f yaw=%.4f pitch=%.4f"
                " fov=%.4f nearZ=%.4f farZ=%.4f moveSpeed=%.4f\n",
                c->position.x, c->position.y, c->position.z,
                c->yaw, c->pitch, c->fov, c->nearZ, c->farZ, c->moveSpeed);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CameraComponent c;
            c.position = { GetF(kv,"px",4.f), GetF(kv,"py",3.f), GetF(kv,"pz",5.f) };
            c.yaw   = GetF(kv, "yaw", -2.47f);
            c.pitch = GetF(kv, "pitch", 0.44f);
            c.fov   = GetF(kv, "fov", 1.047f);
            c.nearZ = GetF(kv, "nearZ", 0.1f);
            c.farZ  = GetF(kv, "farZ", 200.f);
            c.moveSpeed = GetF(kv, "moveSpeed", 10.f);
            w.AddComponent<CameraComponent>(e, c);
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

    // ==== ScriptComponent (INLINE) ====
    reg.Register(std::type_index(typeid(ScriptComponent)), {
        "Script",
        [](World& w, Entity e) { return w.GetComponent<ScriptComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* sc = w.GetComponent<ScriptComponent>(e);
            if (!sc) return;
            ss << "  Script: path=" << PercentEncode(sc->scriptPath)
               << " enabled=" << (sc->enabled ? 1 : 0) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ScriptComponent sc;
            sc.scriptPath = PercentDecode(GetS(kv, "path", ""));
            sc.enabled    = GetI(kv, "enabled", 1) != 0;
            w.AddComponent<ScriptComponent>(e, sc);
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
