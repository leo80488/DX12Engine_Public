#include "ECS/TODSystems.h"
#include "ECS/TODComponents.h"
#include "ECS/Components.h"   // LightData, LightType

#include <algorithm>
#include <cmath>

using namespace DirectX;

// ---------------------------------------------------------------------------
namespace TODUtil
{
    TODConfigComponent* FindConfig(World& w)
    {
        auto* p = w.GetPool<TODConfigComponent>();
        if (!p || p->Data().empty()) return nullptr;
        return &p->Data()[0];
    }
    TODOutputComponent* FindOutput(World& w)
    {
        auto* p = w.GetPool<TODOutputComponent>();
        if (!p || p->Data().empty()) return nullptr;
        return &p->Data()[0];
    }
}

// ---------------------------------------------------------------------------
void TODTickSystem::Update(World& w, float dt)
{
    auto* cfg = TODUtil::FindConfig(w);
    if (!cfg || !cfg->enabled || cfg->timeSpeed == 0.0f) return;
    cfg->timeOfDay += cfg->timeSpeed * dt;
    cfg->timeOfDay -= std::floor(cfg->timeOfDay);   // wrap to [0,1)
}

// ---------------------------------------------------------------------------
void TODEvaluationSystem::Update(World& w)
{
    auto* cfg = TODUtil::FindConfig(w);
    auto* out = TODUtil::FindOutput(w);
    if (!cfg || !out) return;
    if (!cfg->enabled)
    {
        out->dirty = false;   // signal sync systems to skip
        return;
    }

    // ---- Sun direction from hour angle + latitude (declination = 0) --------
    const float H    = (cfg->timeOfDay - 0.5f) * (2.0f * 3.14159265f);
    const float cosH = std::cos(H);
    const float sinH = std::sin(H);
    const float cosL = std::cos(cfg->latitudeRad);
    const float sinL = std::sin(cfg->latitudeRad);

    XMFLOAT3 sunDir;
    sunDir.x = -sinH;            // east at sunrise side
    sunDir.y = cosL * cosH;      // altitude
    sunDir.z = -sinL * cosH;     // north-south
    const float len = std::sqrt(sunDir.x*sunDir.x + sunDir.y*sunDir.y + sunDir.z*sunDir.z);
    if (len > 1e-6f) { sunDir.x /= len; sunDir.y /= len; sunDir.z /= len; }

    const bool sunBelowHorizon = (sunDir.y < 0.0f);

    // ---- Geometric sun (for atmosphere shaders) ----------------------------
    // Below horizon → colour zero so scattering goes dark, no blue daytime
    // dome lingering at night.
    out->sunDirection = sunDir;
    if (sunBelowHorizon)
    {
        out->sunColor = { 0.0f, 0.0f, 0.0f };
    }
    else
    {
        const float elev      = sunDir.y;
        const float daylight  = std::max(0.0f, elev);
        const float tw        = std::exp(-std::max(0.0f, -elev) * 8.0f);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * (0.05f + 0.95f * daylight) * tw
                              * cfg->sunBrightnessScale;
        const float warmth    = 1.0f - std::min(1.0f, daylight * 2.0f);
        out->sunColor = {
            intensity,
            intensity * (1.00f - 0.30f * warmth),
            intensity * (1.00f - 0.55f * warmth) };
    }

    // ---- Moon = -sun. Disk visible slightly past horizon for smooth fade ----
    out->moonDirection = { -sunDir.x, -sunDir.y, -sunDir.z };
    out->moonDiskVisible = (out->moonDirection.y > 0.01f);

    // Moon disk colour: constant cool blue-white so it stays readable through
    // its whole arc. Intensity fade is in the shader's horizon mask.
    constexpr float kMoonDiskTint = 0.1f;
    out->moonColor = { 0.55f * kMoonDiskTint,
                       0.70f * kMoonDiskTint,
                       1.00f * kMoonDiskTint };

    // ---- Active body — sun by day, moon by night, smooth ramp across 0 -----
    auto smoothstep01 = [](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    constexpr float kBand = 0.05f;   // ≈ 2.9°
    const float sunT  = smoothstep01( sunDir.y / kBand);
    const float moonT = smoothstep01(-sunDir.y / kBand);

    XMFLOAT3 sunCol { 0, 0, 0 };
    {
        const float elev      = std::max(0.0f, sunDir.y);
        const float tw        = std::exp(-std::max(0.0f, -sunDir.y) * 8.0f);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * (0.05f + 0.95f * elev) * tw
                              * cfg->sunBrightnessScale;
        const float warmth    = 1.0f - std::min(1.0f, elev * 2.0f);
        sunCol.x = intensity;
        sunCol.y = intensity * (1.0f - 0.30f * warmth);
        sunCol.z = intensity * (1.0f - 0.55f * warmth);
    }

    XMFLOAT3 moonCol { 0, 0, 0 };
    {
        const float moonElev  = -sunDir.y;
        const float moonlight = std::max(0.0f, moonElev);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * cfg->moonIntensityScale
                              * (0.75f + 0.25f * moonlight)
                              * cfg->sunBrightnessScale;
        moonCol.x = intensity * 0.55f;
        moonCol.y = intensity * 0.70f;
        moonCol.z = intensity * 1.00f;
    }

    out->isMoonActive    = sunBelowHorizon;
    out->activeDirection = sunBelowHorizon
        ? XMFLOAT3{ -sunDir.x, -sunDir.y, -sunDir.z }
        : sunDir;
    out->activeColor.x = sunCol.x * sunT + moonCol.x * moonT;
    out->activeColor.y = sunCol.y * sunT + moonCol.y * moonT;
    out->activeColor.z = sunCol.z * sunT + moonCol.z * moonT;
    out->dirty = true;
}

// ---------------------------------------------------------------------------
template <typename Tag>
static Entity FirstEntityWithTag(World& w)
{
    auto* p = w.GetPool<Tag>();
    if (!p || p->Entities().empty()) return NullEntity;
    return p->Entities()[0];
}

// ---------------------------------------------------------------------------
void TODSunSyncSystem::Update(World& w)
{
    auto* out = TODUtil::FindOutput(w);
    auto* cfg = TODUtil::FindConfig(w);
    if (!cfg || !out || !cfg->enabled) return;

    Entity e = FirstEntityWithTag<SunLightTag>(w);
    if (e == NullEntity) return;
    LightData* ld = w.GetComponent<LightData>(e);
    if (!ld) return;

    ld->type = LightType::Directional;
    // LightData.direction uses light-travel convention (from sun → ground),
    // so negate sunDirection (which points TOWARDS the sun).
    ld->direction = { -out->sunDirection.x, -out->sunDirection.y, -out->sunDirection.z };

    if (out->isMoonActive)
    {
        // Sun below horizon — moon entity drives lighting, zero sun contribution.
        ld->color     = { 0.f, 0.f, 0.f };
        ld->intensity = 0.f;
    }
    else
    {
        // activeColor already bakes in basePeak * brightness ramp + warmth.
        ld->color     = out->activeColor;
        ld->intensity = 1.f;
    }
}

// ---------------------------------------------------------------------------
void TODMoonSyncSystem::Update(World& w)
{
    auto* out = TODUtil::FindOutput(w);
    auto* cfg = TODUtil::FindConfig(w);
    if (!cfg || !out || !cfg->enabled) return;

    Entity e = FirstEntityWithTag<MoonLightTag>(w);
    if (e == NullEntity) return;
    LightData* ld = w.GetComponent<LightData>(e);
    if (!ld) return;

    ld->type = LightType::Directional;
    ld->direction = { -out->moonDirection.x, -out->moonDirection.y, -out->moonDirection.z };

    if (out->isMoonActive)
    {
        ld->color     = out->activeColor;
        ld->intensity = 1.f;
    }
    else
    {
        ld->color     = { 0.f, 0.f, 0.f };
        ld->intensity = 0.f;
    }
}

// ---------------------------------------------------------------------------
void TODAtmosphereSyncSystem::Update(World&)
{
    // Reserved for future TOD-driven atmosphere parameters (sky tint curve,
    // mist density at dawn/dusk, etc). Today the AtmosphereComponent fields
    // are author-owned so this system is a deliberate no-op — kept in place
    // so the system list matches the design and the hook exists when
    // curve-driven atmosphere lands.
}
