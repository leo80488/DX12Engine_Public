#include "UI/Tween.h"
#include <algorithm>
#include <cmath>

namespace UI
{
    // ---- Easing curves -------------------------------------------------------
    // t is normalised [0,1]. Output is also expected in [0,1] for normal cases
    // (Back / Elastic intentionally over-/under-shoot for visual snap).
    float ApplyEasing(Easing e, float t)
    {
        t = std::clamp(t, 0.f, 1.f);
        switch (e)
        {
        case Easing::Linear:     return t;
        case Easing::InQuad:     return t * t;
        case Easing::OutQuad:    return 1.f - (1.f - t) * (1.f - t);
        case Easing::InOutQuad:
            return (t < 0.5f) ? 2.f * t * t
                              : 1.f - std::powf(-2.f * t + 2.f, 2.f) * 0.5f;
        case Easing::InCubic:    return t * t * t;
        case Easing::OutCubic:
        {
            const float inv = 1.f - t;
            return 1.f - inv * inv * inv;
        }
        case Easing::InOutCubic:
            return (t < 0.5f) ? 4.f * t * t * t
                              : 1.f - std::powf(-2.f * t + 2.f, 3.f) * 0.5f;
        case Easing::OutBack:
        {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.f;
            const float inv = t - 1.f;
            return 1.f + c3 * inv * inv * inv + c1 * inv * inv;
        }
        case Easing::OutElastic:
        {
            constexpr float c4 = 6.283185307f / 3.f;
            if (t == 0.f || t == 1.f) return t;
            return std::powf(2.f, -10.f * t)
                 * std::sin((t * 10.f - 0.75f) * c4) + 1.f;
        }
        case Easing::OutBounce:
        {
            constexpr float n1 = 7.5625f;
            constexpr float d1 = 2.75f;
            if (t < 1.f / d1)         return n1 * t * t;
            if (t < 2.f / d1)       { const float u = t - 1.5f / d1; return n1 * u * u + 0.75f; }
            if (t < 2.5f / d1)      { const float u = t - 2.25f / d1; return n1 * u * u + 0.9375f; }
            { const float u = t - 2.625f / d1; return n1 * u * u + 0.984375f; }
        }
        }
        return t;
    }

    // ---- TweenSystem ---------------------------------------------------------

    TweenSystem& TweenSystem::Get()
    {
        static TweenSystem s;
        return s;
    }

    size_t TweenSystem::AddTween(Tween t)
    {
        m_tweens.push_back(std::move(t));
        return m_tweens.size() - 1;
    }

    void TweenSystem::TweenVec2(Vec2* storage, Vec2 from, Vec2 to,
                                float duration, Easing easing, float delay,
                                std::function<void()> onDone)
    {
        if (!storage) return;
        Tween a{};
        a.storage   = &storage->x;
        a.fromValue = from.x; a.toValue = to.x;
        a.duration  = duration; a.delay = delay; a.easing = easing;
        AddTween(std::move(a));

        Tween b{};
        b.storage   = &storage->y;
        b.fromValue = from.y; b.toValue = to.y;
        b.duration  = duration; b.delay = delay; b.easing = easing;
        // Only fire onDone on the second axis so we don't double-call.
        b.onDone    = std::move(onDone);
        AddTween(std::move(b));
    }

    void TweenSystem::TweenColor(Color32* storage, Color32 from, Color32 to,
                                 float duration, Easing easing, float delay,
                                 std::function<void()> onDone)
    {
        if (!storage) return;
        // Spawn a backer slot that holds the 4 working floats.
        m_colorBackers.push_back({ storage,
            { static_cast<float>(from.rgba       & 0xFFu),
              static_cast<float>((from.rgba >> 8) & 0xFFu),
              static_cast<float>((from.rgba >> 16) & 0xFFu),
              static_cast<float>((from.rgba >> 24) & 0xFFu) }, 0u });
        ColorBacker& bk = m_colorBackers.back();
        const float toR = static_cast<float>(to.rgba       & 0xFFu);
        const float toG = static_cast<float>((to.rgba >> 8) & 0xFFu);
        const float toB = static_cast<float>((to.rgba >> 16) & 0xFFu);
        const float toA = static_cast<float>((to.rgba >> 24) & 0xFFu);

        // Capture backer pointer (m_colorBackers is stable across this call).
        ColorBacker* bkPtr = &bk;
        auto bakeBack = [bkPtr](uint32_t ch) {
            return [bkPtr, ch]() {
                bkPtr->channelDoneMask |= (1u << ch);
                if (bkPtr->channelDoneMask == 0xFu)
                {
                    const auto r = static_cast<uint32_t>(std::clamp(bkPtr->rgba[0], 0.f, 255.f));
                    const auto g = static_cast<uint32_t>(std::clamp(bkPtr->rgba[1], 0.f, 255.f));
                    const auto b = static_cast<uint32_t>(std::clamp(bkPtr->rgba[2], 0.f, 255.f));
                    const auto a = static_cast<uint32_t>(std::clamp(bkPtr->rgba[3], 0.f, 255.f));
                    bkPtr->dst->rgba = r | (g << 8) | (b << 16) | (a << 24);
                }
            };
        };

        Tween tr{}; tr.storage = &bk.rgba[0]; tr.fromValue = bk.rgba[0]; tr.toValue = toR;
        tr.duration = duration; tr.delay = delay; tr.easing = easing;
        tr.onDone = bakeBack(0);
        AddTween(std::move(tr));
        Tween tg{}; tg.storage = &bk.rgba[1]; tg.fromValue = bk.rgba[1]; tg.toValue = toG;
        tg.duration = duration; tg.delay = delay; tg.easing = easing;
        tg.onDone = bakeBack(1);
        AddTween(std::move(tg));
        Tween tb{}; tb.storage = &bk.rgba[2]; tb.fromValue = bk.rgba[2]; tb.toValue = toB;
        tb.duration = duration; tb.delay = delay; tb.easing = easing;
        tb.onDone = bakeBack(2);
        AddTween(std::move(tb));
        Tween ta{}; ta.storage = &bk.rgba[3]; ta.fromValue = bk.rgba[3]; ta.toValue = toA;
        ta.duration = duration; ta.delay = delay; ta.easing = easing;
        // Only the last channel fires the user callback (after the bake-back).
        auto userDone = std::move(onDone);
        ta.onDone = [bkPtr, ud = std::move(userDone)]() {
            bkPtr->channelDoneMask |= (1u << 3);
            // bake fires whatever channel completed last writes through.
            if (ud) ud();
        };
        AddTween(std::move(ta));
    }

    void TweenSystem::Tick(float dt)
    {
        for (auto& t : m_tweens)
        {
            if (!t.active || t.done) continue;
            // Cancel if guard handle no longer resolves.
            if (t.guardHandle.IsValid()
                && WidgetRegistry::Get().Get(t.guardHandle) == nullptr)
            { t.done = true; t.active = false; continue; }
            if (!t.storage) { t.done = true; t.active = false; continue; }

            if (t.delay > 0.f) { t.delay -= dt; if (t.delay > 0.f) continue; t.delay = 0.f; }

            t.elapsed += dt;
            const float u = (t.duration <= 0.f) ? 1.f
                                                : std::clamp(t.elapsed / t.duration, 0.f, 1.f);
            const float k = ApplyEasing(t.easing, u);
            *t.storage = t.fromValue + (t.toValue - t.fromValue) * k;
            if (u >= 1.f)
            {
                t.done = true;
                t.active = false;
                if (t.onDone) t.onDone();
            }
        }

        // Compact: drop completed entries.
        m_tweens.erase(
            std::remove_if(m_tweens.begin(), m_tweens.end(),
                           [](const Tween& t) { return t.done; }),
            m_tweens.end());

        // Color backers stay alive as long as ANY of their tweens is alive,
        // but since color tweens always come in groups of 4 with the same
        // duration+delay+easing, simply drop backers when no tween points
        // into their rgba[]. We just drop after the bake-callback fired.
        m_colorBackers.erase(
            std::remove_if(m_colorBackers.begin(), m_colorBackers.end(),
                           [](const ColorBacker& b) { return b.channelDoneMask == 0xFu; }),
            m_colorBackers.end());
    }

    size_t TweenSystem::ActiveCount() const
    {
        size_t n = 0;
        for (const auto& t : m_tweens) if (t.active) ++n;
        return n;
    }

} // namespace UI
