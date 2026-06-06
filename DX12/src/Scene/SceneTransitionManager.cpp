#include "Scene/SceneTransitionManager.h"

#include "Scene/IGameMode.h"
#include "UI/UIDrawList.h"
#include "UI/Font.h"

#include <cstdint>

namespace
{
    inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    // Smoothstep — soft ease at both ends of a fade.
    inline float SmoothStep(float t) { t = Clamp01(t); return t * t * (3.f - 2.f * t); }

    inline uint8_t ToByte(float a01) { return static_cast<uint8_t>(Clamp01(a01) * 255.f + 0.5f); }
}

SceneTransitionManager::~SceneTransitionManager() = default;

void SceneTransitionManager::Begin(std::unique_ptr<IGameMode> target)
{
    // A transition already owns the screen, or nothing to switch to.
    if (m_phase != Phase::Idle || !target) return;

    m_target        = std::move(target);
    m_phase         = Phase::FadeOut;
    m_elapsed       = 0.f;
    m_alpha         = 0.f;
    m_progress      = 0.f;
    m_swapRequested = false;
}

void SceneTransitionManager::Tick(float dt, const ReplaceModeFn& replaceMode)
{
    if (dt < 0.f) dt = 0.f;

    switch (m_phase)
    {
    case Phase::Idle:
        return;

    case Phase::FadeOut:
    {
        m_elapsed += dt;
        const float t = (fadeOutSeconds <= 0.f) ? 1.f : Clamp01(m_elapsed / fadeOutSeconds);
        m_alpha    = SmoothStep(t);
        m_progress = 0.15f * t; // a little movement while the old scene darkens
        if (t >= 1.f)
        {
            m_alpha         = 1.f; // fully black — safe to swap now
            m_phase         = Phase::Loading;
            m_elapsed       = 0.f;
            m_swapRequested = false;
        }
        break;
    }

    case Phase::Loading:
    {
        m_alpha = 1.f; // hold full black across the stall
        if (!m_swapRequested)
        {
            // One fully-black frame is already on screen. NOW trigger the
            // Pop(old)+Push(new->Init blocking load) swap. App drains the
            // request this same frame, so the freeze happens behind black.
            if (replaceMode && m_target) replaceMode(std::move(m_target));
            m_swapRequested = true;
            m_progress      = 0.6f;
        }
        else
        {
            // Swap + blocking load finished last frame; the new scene is live
            // and rendering under the black. Reveal it.
            m_progress = 1.f;
            m_phase    = Phase::FadeIn;
            m_elapsed  = 0.f; // discard the long load frame's dt — start clean
        }
        break;
    }

    case Phase::FadeIn:
    {
        m_elapsed += dt;
        const float t = (fadeInSeconds <= 0.f) ? 1.f : Clamp01(m_elapsed / fadeInSeconds);
        m_alpha    = 1.f - SmoothStep(t);
        m_progress = 1.f;
        if (t >= 1.f)
        {
            m_alpha = 0.f;
            m_phase = Phase::Idle;
        }
        break;
    }
    }
}

void SceneTransitionManager::DrawOverlay(UI::UIDrawList& dl, unsigned int viewportW,
                                         unsigned int viewportH) const
{
    if (m_phase == Phase::Idle && m_alpha <= 0.f) return;
    if (viewportW == 0 || viewportH == 0) return;

    using UI::Color32;
    using UI::Vec2;

    const float w = static_cast<float>(viewportW);
    const float h = static_cast<float>(viewportH);

    // 1. Full-screen black fade quad.
    dl.AddRectFilled(Vec2{ 0.f, 0.f }, Vec2{ w, h }, Color32(0, 0, 0, ToByte(m_alpha)));

    // Bar + text only appear once the screen is mostly dark, and ramp with the
    // fade so they never bleed over the old/new scene during the fades.
    const float contentA = Clamp01((m_alpha - 0.55f) / 0.45f);
    if (contentA <= 0.f) return;

    // 2. Progress bar — centred, lower third.
    const float barW = w * 0.32f;
    const float barH = 8.f;
    const float barX = (w - barW) * 0.5f;
    const float barY = h * 0.78f;

    dl.AddRectFilled(Vec2{ barX, barY }, Vec2{ barX + barW, barY + barH },
                     Color32(50, 50, 55, ToByte(contentA)));                  // track
    const float fillW = barW * Clamp01(m_progress);
    if (fillW > 0.f)
        dl.AddRectFilled(Vec2{ barX, barY }, Vec2{ barX + fillW, barY + barH },
                         Color32(235, 235, 240, ToByte(contentA)));           // fill
    dl.AddRect(Vec2{ barX, barY }, Vec2{ barX + barW, barY + barH },
               Color32(120, 120, 130, ToByte(contentA)), 1.f);               // border

    // 3. "Now Loading..." text above the bar (silently skipped if no font).
    UI::Font& font = UI::DefaultFont();
    if (font.IsReady())
    {
        const char* msg = "Now Loading...";
        const Vec2  ts  = font.MeasureText(msg);
        const Vec2  pos{ (w - ts.x) * 0.5f, barY - ts.y - 14.f };
        font.RenderText(dl, pos, Color32(255, 255, 255, ToByte(contentA)), msg);
    }
}
