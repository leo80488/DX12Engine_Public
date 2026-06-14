#include "PostProcess/PostProcessRuntime.h"

#include <algorithm>

namespace PostProcess
{

Runtime& Runtime::Get()
{
    static Runtime s_instance;
    return s_instance;
}

uint64_t Runtime::PushOverride(PostProcessOverride ov)
{
    ov.id      = m_nextId++;
    ov.elapsed = 0.0f;
    overrides.push_back(std::move(ov));
    return overrides.back().id;
}

void Runtime::RemoveOverride(uint64_t id)
{
    overrides.erase(
        std::remove_if(overrides.begin(), overrides.end(),
                       [id](const PostProcessOverride& o) { return o.id == id; }),
        overrides.end());
}

// Piecewise-linear fade: 0→1 over fadeIn, 1 for hold, 1→0 over fadeOut.
// A negative hold means "persistent" — stay at 1 after fadeIn until removed.
float EvaluateOverrideEnvelope(const PostProcessOverride& o)
{
    constexpr float kEps = 1.0e-4f;
    const float e = o.elapsed;
    if (e <= 0.0f) return (o.fadeIn > kEps) ? 0.0f : 1.0f;

    if (e < o.fadeIn) return e / std::max(o.fadeIn, kEps);
    float t = e - o.fadeIn;

    if (o.hold < 0.0f) return 1.0f;   // persistent
    if (t < o.hold) return 1.0f;
    t -= o.hold;

    if (t < o.fadeOut) return 1.0f - (t / std::max(o.fadeOut, kEps));
    return 0.0f;
}

bool IsOverrideExpired(const PostProcessOverride& o)
{
    if (o.hold < 0.0f) return false;  // persistent until manually removed
    return o.elapsed >= (o.fadeIn + o.hold + o.fadeOut);
}

} // namespace PostProcess
