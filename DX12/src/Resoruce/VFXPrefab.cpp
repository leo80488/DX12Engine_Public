#include "Resource/VFXPrefab.h"

#include "System/Log.h"

#include <DirectXMath.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace Resource
{

namespace
{
    // ---- Tokens ------------------------------------------------------------

    // KV map populated from a "PARTICLE k1=v1 k2=v2 ..." line. Mirrors the
    // same per-key set ComponentSerializers writes for ParticleEmitterComponent
    // so designers can copy a line straight out of a saved .iscn into a new
    // .ivfx and have it work.
    using KV = std::unordered_map<std::string, std::string>;

    KV ParseInlineKV(const std::string& body)
    {
        KV out;
        std::istringstream iss(body);
        std::string tok;
        while (iss >> tok) {
            const auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            out.emplace(tok.substr(0, eq), tok.substr(eq + 1));
        }
        return out;
    }

    int   GetI(const KV& kv, const char* k, int def) {
        auto it = kv.find(k); return (it == kv.end()) ? def : std::atoi(it->second.c_str());
    }
    float GetF(const KV& kv, const char* k, float def) {
        auto it = kv.find(k); return (it == kv.end()) ? def : static_cast<float>(std::atof(it->second.c_str()));
    }
    std::string GetS(const KV& kv, const char* k) {
        auto it = kv.find(k); return (it == kv.end()) ? std::string{} : it->second;
    }

    void ParseVec3(const KV& kv, const char* k, DirectX::XMFLOAT3& out)
    {
        auto it = kv.find(k);
        if (it == kv.end()) return;
        sscanf_s(it->second.c_str(), "%f_%f_%f", &out.x, &out.y, &out.z);
    }
    void ParseVec4(const KV& kv, const char* k, DirectX::XMFLOAT4& out)
    {
        auto it = kv.find(k);
        if (it == kv.end()) return;
        sscanf_s(it->second.c_str(), "%f_%f_%f_%f", &out.x, &out.y, &out.z, &out.w);
    }

    // ---- Particle KV → component ------------------------------------------

    ParticleEmitterComponent ParticleFromKV(const KV& kv)
    {
        ParticleEmitterComponent p;
        p.enabled        = GetI(kv, "enabled", 1) != 0;
        p.spawnRate      = GetF(kv, "spawnRate",     p.spawnRate);
        p.startLifetime  = GetF(kv, "startLifetime", p.startLifetime);
        p.startSize      = GetF(kv, "startSize",     p.startSize);

        ParseVec3(kv, "velMin",         p.velocityMin);
        ParseVec3(kv, "velMax",         p.velocityMax);
        ParseVec4(kv, "startColor",     p.startColor);
        ParseVec4(kv, "endColor",       p.endColor);
        ParseVec3(kv, "gravity",        p.gravity);
        ParseVec3(kv, "coneDir",        p.coneDirection);
        ParseVec3(kv, "boxHalfExtents", p.boxHalfExtents);
        ParseVec3(kv, "circleNormal",   p.circleNormal);

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

        // texturePath remains a path — ParticleSystem resolves bindless on
        // first tick. .ivfx never persists bindless indices (would be
        // process-local) or meshSourceEntity (cross-instance Entity IDs
        // don't survive prefab cloning).
        p.texturePath        = GetS(kv, "texturePath");
        p.textureBindlessIdx = -1;
        p.textureGpuHandle   = 0;
        p.meshSourceEntity   = 0;

        return p;
    }

    // ---- Trail KV → component (mirrors TrailComponent serialized fields) ---

    TrailComponent TrailFromKV(const KV& kv)
    {
        TrailComponent t;
        t.width             = GetF(kv, "width",   t.width);
        t.maxAge            = GetF(kv, "maxAge",  t.maxAge);
        t.minSampleDistance = GetF(kv, "minDist", t.minSampleDistance);
        ParseVec4(kv, "startColor", t.startColor);
        ParseVec4(kv, "endColor",   t.endColor);
        t.enabled           = GetI(kv, "enabled", 1) != 0;
        return t;
    }

    // ---- Beam KV → spec (control points come from separate BP lines) ------

    void BeamFromKV(const KV& kv, BeamVFXSpec& b)
    {
        b.beam.globalRadiusScale = GetF(kv, "radiusScale", b.beam.globalRadiusScale);
        b.beam.wobbleAmplitude   = GetF(kv, "wobbleAmp",   b.beam.wobbleAmplitude);
        b.beam.wobbleSpeed       = GetF(kv, "wobbleSpeed", b.beam.wobbleSpeed);
        std::string sh = GetS(kv, "shader");
        if (!sh.empty()) b.shaderPath = sh;
        std::string blend = GetS(kv, "blend");
        if (!blend.empty()) b.additive = (blend != "opaque" && blend != "core");
    }

    // ---- Decal / Tracer / Afterimage / Mesh KV → spec ---------------------

    void DecalFromKV(const KV& kv, DecalVFXSpec& d)
    {
        std::string m = GetS(kv, "material");
        if (m.empty()) m = GetS(kv, "materialName");
        d.materialName    = m;
        d.sizeX           = GetF(kv, "sizeX", d.sizeX);
        d.sizeY           = GetF(kv, "sizeY", d.sizeY);
        d.depth           = GetF(kv, "depth", d.depth);
        d.lifetime        = GetF(kv, "life",  d.lifetime);
        d.fadeOutDuration = GetF(kv, "fade",  d.fadeOutDuration);
        d.rollZ           = GetF(kv, "roll",  d.rollZ);
        ParseVec4(kv, "tint", d.tintOverride);
    }

    void TracerFromKV(const KV& kv, TracerVFXSpec& t)
    {
        ParseVec4(kv, "color", t.color);
        t.width    = GetF(kv, "width", t.width);
        t.lifetime = GetF(kv, "life",  t.lifetime);
        t.length   = GetF(kv, "len",   t.length);
        ParseVec3(kv, "dir", t.direction);
        const int noise = GetI(kv, "noise", -1);
        if (noise >= 0) t.noiseTexBindless = static_cast<uint32_t>(noise);
    }

    void AfterimageFromKV(const KV& kv, AfterimageVFXSpec& a)
    {
        a.lifetime = GetF(kv, "life", a.lifetime);
        ParseVec4(kv, "color", a.color);
    }

    void MeshFromKV(const KV& kv, MeshVFXSpec& m)
    {
        m.meshPath      = GetS(kv, "mesh");
        m.materialPath  = GetS(kv, "material");
        m.spinDegPerSec = GetF(kv, "spinDegPerSec", m.spinDegPerSec);
        ParseVec3(kv, "spinAxis", m.spinAxis);
        m.castShadow    = GetI(kv, "castShadow", 0) != 0;
    }

    // ---- Type token → enum (case-insensitive) -----------------------------

    bool VFXTypeFromName(std::string s, VFXEmitterType& out)
    {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if      (s == "PARTICLE")   out = VFXEmitterType::Particle;
        else if (s == "TRAIL")      out = VFXEmitterType::Trail;
        else if (s == "BEAM")       out = VFXEmitterType::Beam;
        else if (s == "DECAL")      out = VFXEmitterType::Decal;
        else if (s == "TRACER")     out = VFXEmitterType::Tracer;
        else if (s == "AFTERIMAGE") out = VFXEmitterType::Afterimage;
        else if (s == "MESH")       out = VFXEmitterType::Mesh;
        else return false;
        return true;
    }

    // ---- Trim whitespace + drop '#' comments ------------------------------

    std::string Sanitize(const std::string& raw)
    {
        std::string s = raw;
        if (auto h = s.find('#'); h != std::string::npos) s.resize(h);
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last  = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }
}

bool LoadVFXPrefab(const std::string& path, VFXPrefab& out)
{
    out.emitters.clear();
    out.defaultDuration = 2.0f;

    std::ifstream f(path);
    if (!f) {
        LOG_WARNING("VFXPrefab: cannot open '%s'", path.c_str());
        return false;
    }

    int    version       = -1;
    bool   inEmitter     = false;
    VFXEmitterSpec cur;

    DirectX::XMFLOAT3 curEulerDeg{ 0, 0, 0 };
    bool eulerSeen = false;

    auto commitEulerIfNeeded = [&]() {
        if (!eulerSeen) return;
        using namespace DirectX;
        const float kDeg2Rad = XM_PI / 180.f;
        XMVECTOR q = XMQuaternionRotationRollPitchYaw(
            curEulerDeg.x * kDeg2Rad,
            curEulerDeg.y * kDeg2Rad,
            curEulerDeg.z * kDeg2Rad);
        XMStoreFloat4(&cur.localTransform.rotation, q);
        eulerSeen = false;
    };

    auto flushEmitter = [&]() {
        if (!inEmitter) return;
        commitEulerIfNeeded();
        out.emitters.push_back(std::move(cur));
        cur = VFXEmitterSpec{};
        inEmitter = false;
    };

    std::string raw;
    while (std::getline(f, raw))
    {
        const std::string line = Sanitize(raw);
        if (line.empty()) continue;

        // Split off the first whitespace-delimited token as the directive.
        const auto sp = line.find_first_of(" \t");
        const std::string head = (sp == std::string::npos) ? line : line.substr(0, sp);
        const std::string body = (sp == std::string::npos) ? std::string{} : line.substr(sp + 1);

        if (head == "VFX") {
            version = std::atoi(body.c_str());
            if (version != 1) {
                LOG_WARNING("VFXPrefab: '%s' uses unsupported version %d (expected 1)",
                            path.c_str(), version);
                // Don't abort — keep parsing in case future versions stay
                // wire-compatible. Worst case the file is empty.
            }
        }
        else if (head == "DURATION") {
            out.defaultDuration = static_cast<float>(std::atof(body.c_str()));
        }
        else if (head == "EMITTER") {
            flushEmitter();
            inEmitter = true;
            cur = VFXEmitterSpec{};
            curEulerDeg = { 0, 0, 0 };
            eulerSeen   = false;
            // Optional type token directly on the EMITTER line ("EMITTER TRAIL").
            if (!body.empty()) {
                VFXEmitterType t;
                if (VFXTypeFromName(body, t)) cur.type = t;
            }
        }
        else if (head == "TYPE" && inEmitter) {
            VFXEmitterType t;
            if (VFXTypeFromName(body, t)) cur.type = t;
            else LOG_WARNING("VFXPrefab: '%s' unknown emitter TYPE '%s'", path.c_str(), body.c_str());
        }
        else if ((head == "DELAY") && inEmitter) {
            cur.startDelay = static_cast<float>(std::atof(body.c_str()));
        }
        else if ((head == "DUR" || head == "LIFE") && inEmitter) {
            cur.durationOverride = static_cast<float>(std::atof(body.c_str()));
        }
        else if (head == "P" && inEmitter) {
            sscanf_s(body.c_str(), "%f %f %f",
                     &cur.localTransform.translation.x,
                     &cur.localTransform.translation.y,
                     &cur.localTransform.translation.z);
        }
        else if (head == "R" && inEmitter) {
            sscanf_s(body.c_str(), "%f %f %f",
                     &curEulerDeg.x, &curEulerDeg.y, &curEulerDeg.z);
            eulerSeen = true;
        }
        else if (head == "S" && inEmitter) {
            sscanf_s(body.c_str(), "%f %f %f",
                     &cur.localTransform.scale.x,
                     &cur.localTransform.scale.y,
                     &cur.localTransform.scale.z);
        }
        // ---- Per-type body directives. A recognized directive ALSO sets the
        //      emitter type, so the type token is optional when the content
        //      line is unambiguous (forgiving-loader philosophy). ----------
        else if (head == "PARTICLE" && inEmitter) {
            cur.type     = VFXEmitterType::Particle;
            cur.particle = ParticleFromKV(ParseInlineKV(body));
        }
        else if (head == "TRAIL" && inEmitter) {
            cur.type  = VFXEmitterType::Trail;
            cur.trail = TrailFromKV(ParseInlineKV(body));
        }
        else if (head == "BEAM" && inEmitter) {
            cur.type = VFXEmitterType::Beam;
            BeamFromKV(ParseInlineKV(body), cur.beam);
        }
        else if (head == "BP" && inEmitter) {
            // Beam control point: "x y z radius [r g b a]".
            BeamControlPoint cp;
            float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
            const int got = sscanf_s(body.c_str(), "%f %f %f %f %f %f %f %f",
                                     &cp.position.x, &cp.position.y, &cp.position.z,
                                     &cp.radius, &r, &g, &b, &a);
            if (got >= 3) {
                cp.colorTint = { r, g, b, a };
                cur.beam.beam.controlPoints.push_back(cp);
                cur.type = VFXEmitterType::Beam;
            }
        }
        else if (head == "DECAL" && inEmitter) {
            cur.type = VFXEmitterType::Decal;
            DecalFromKV(ParseInlineKV(body), cur.decal);
        }
        else if (head == "TRACER" && inEmitter) {
            cur.type = VFXEmitterType::Tracer;
            TracerFromKV(ParseInlineKV(body), cur.tracer);
        }
        else if (head == "AFTERIMAGE" && inEmitter) {
            cur.type = VFXEmitterType::Afterimage;
            AfterimageFromKV(ParseInlineKV(body), cur.afterimage);
        }
        else if (head == "MESH" && inEmitter) {
            cur.type = VFXEmitterType::Mesh;
            MeshFromKV(ParseInlineKV(body), cur.mesh);
        }
        // Unknown directives are silently ignored — designers can put extra
        // notes in files without breaking the loader.
    }
    flushEmitter();

    if (out.emitters.empty()) {
        LOG_WARNING("VFXPrefab: '%s' parsed but contains no EMITTER blocks",
                    path.c_str());
    }
    return true;
}

} // namespace Resource
