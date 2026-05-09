#pragma once

// Tween — minimal float-target animation system for UI.
//
// Drives a single mutable float / Vec2 / Color32 from start to end over a
// duration with an easing function. Designed to be cheap (no allocator
// surprise) and Lua-friendly (callback at completion).
//
// Lifetime: TweenSystem owns active tweens; UISystem ticks it once per
// frame. Cancel-on-destroy: tweens hold WidgetHandle, so a destroyed
// widget invalidates the target and the tween becomes a silent no-op.

#include "UI/UIDrawList.h"
#include "UI/WidgetRegistry.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace UI
{
    enum class Easing : uint8_t
    {
        Linear,
        InQuad, OutQuad, InOutQuad,
        InCubic, OutCubic, InOutCubic,
        OutBack, OutElastic, OutBounce,
    };

    float ApplyEasing(Easing e, float t);

    // ---- Generic tween descriptor -------------------------------------------
    // Each tween animates a `float` storage location. Multi-component (Vec2,
    // Color32) tweens decompose into multiple Tween instances.
    struct Tween
    {
        // The storage to write into. Must outlive the tween (use widget
        // members owned by the registry — handle nulls cancel automatically).
        float*    storage = nullptr;
        // Optional handle: cancel the tween if the widget gets freed.
        WidgetHandle  guardHandle{};

        float     fromValue = 0.f;
        float     toValue   = 0.f;
        float     elapsed   = 0.f;
        float     duration  = 0.5f;
        float     delay     = 0.f;
        Easing    easing    = Easing::OutCubic;
        bool      active    = true;
        bool      done      = false;

        // Optional one-shot finished callback (Lua bind: function() end).
        std::function<void()> onDone;
    };

    class TweenSystem
    {
    public:
        // Add a float-target tween. Returns the tween's slot index.
        size_t AddTween(Tween t);

        // Convenience: animate a Vec2 (decomposes into 2 tweens).
        void TweenVec2(Vec2* storage,
                       Vec2 from, Vec2 to,
                       float duration, Easing easing = Easing::OutCubic,
                       float delay = 0.f,
                       std::function<void()> onDone = {});

        // Convenience: animate Color32 (decomposes into 4 tweens on a tmp
        // float[4] and writes back each tick — handles channel quantisation).
        void TweenColor(Color32* storage,
                        Color32 from, Color32 to,
                        float duration, Easing easing = Easing::OutCubic,
                        float delay = 0.f,
                        std::function<void()> onDone = {});

        // Cancel every tween whose guard handle is invalid (widget destroyed)
        // and every tween that completed last frame.
        void Tick(float dt);

        // Drop all tweens (e.g. on scene transition).
        void Clear() { m_tweens.clear(); }

        size_t ActiveCount() const;

        static TweenSystem& Get();

    private:
        std::vector<Tween> m_tweens;

        // For Color32 tweens we shadow the four channels in a side array so
        // we can quantise back to 0..255 each tick after fractional easing.
        struct ColorBacker
        {
            Color32* dst       = nullptr;
            float    rgba[4]   = { 0, 0, 0, 0 };
            uint32_t channelDoneMask = 0;
        };
        std::vector<ColorBacker> m_colorBackers;
    };

} // namespace UI
