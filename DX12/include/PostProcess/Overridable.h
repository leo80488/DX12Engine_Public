#pragma once

// PostProcess::Overridable<T> — the lowest-level data primitive of the
// post-process volume system. Mirrors Unreal's bOverride_X / Unity's
// overrideState: every authored property carries an explicit "do I override
// this?" flag alongside its value.
//
// Blend rule (see ResolvedPostProcessSettings.h): ONLY properties whose
// overrideState == true participate in the lerp; properties left untouched
// let the lower-priority layer (or the engine default) show through.
//
// PostProcessProfile is built entirely from Overridable<T> members; the flat
// ResolvedPostProcessSettings holds the same fields as bare T (no flag).

#include <DirectXMath.h>
#include <cmath>
#include <cstdint>

namespace PostProcess
{

template <typename T>
struct Overridable
{
    bool overrideState = false;   // does this layer override the property?
    T    value{};                 // the authored value (only used when overriding)

    Overridable() = default;
    explicit Overridable(const T& v, bool ov = true) : overrideState(ov), value(v) {}

    // Convenience: set value AND mark as overriding in one call (editor / Lua).
    void Set(const T& v) { value = v; overrideState = true; }
    void Clear()         { overrideState = false; }
};

// ---------------------------------------------------------------------------
// Typed blend helpers — used by the X-macro resolve loop. Kept here so both
// the volume blend (BlendProfileInto) and the override-stack blend share one
// definition with identical semantics for the same type.
// ---------------------------------------------------------------------------

// Bool resolution: snap to the override side once weight crosses 0.5 so
// toggles feel decisive rather than randomly flickering mid-transition.
inline bool BlendValue(bool a, bool b, float t) { return (t >= 0.5f) ? b : a; }

inline float BlendValue(float a, float b, float t) { return a + t * (b - a); }

inline DirectX::XMFLOAT3 BlendValue(const DirectX::XMFLOAT3& a,
                                    const DirectX::XMFLOAT3& b, float t)
{
    return DirectX::XMFLOAT3{
        a.x + t * (b.x - a.x),
        a.y + t * (b.y - a.y),
        a.z + t * (b.z - a.z),
    };
}

// Integer counts (e.g. lens-flare ghost count) lerp in float space then round.
inline uint32_t BlendValue(uint32_t a, uint32_t b, float t)
{
    const float r = static_cast<float>(a) + t * (static_cast<float>(b) - static_cast<float>(a));
    return static_cast<uint32_t>(std::lround(r));
}

} // namespace PostProcess
