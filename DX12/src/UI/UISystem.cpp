#include "UI/UISystem.h"
#include "UI/UIComponents.h"
#include "UI/Tween.h"
#include "UI/Font.h"
#include "ECS/ECS.h"
#include "System/Log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

// SCREEN-SPACE UI ONLY.  World-space UI (HP bars, name plates, damage
// numbers) is implemented in UI/WorldSpaceUI.h + WorldSpaceUISystem +
// WorldUIBillboardPass — completely separate render path, completely
// separate components.

namespace UI
{
    static void DispatchPointerEvents(Widget* root,
                                      UIInputState& s,
                                      bool& wantsCapture,
                                      Widget*& outClickedDown)
    {
        outClickedDown = nullptr;
        if (!root) return;

        if (s.mousePos.x != s.prevPos.x || s.mousePos.y != s.prevPos.y)
        {
            UIEvent e;
            e.type = UIEvent::Type::MouseMove;
            e.mousePos = s.mousePos;
            root->DispatchEvent(e);
        }

        auto edge = [&](bool now, bool prev, uint8_t button)
        {
            if (now && !prev)
            {
                UIEvent e;
                e.type = UIEvent::Type::MouseDown;
                e.mousePos = s.mousePos;
                e.button = button;
                Widget* hit = root->DispatchEvent(e);
                if (hit) {
                    wantsCapture = true;
                    if (button == 0) outClickedDown = hit;
                }
            }
            else if (!now && prev)
            {
                UIEvent e;
                e.type = UIEvent::Type::MouseUp;
                e.mousePos = s.mousePos;
                e.button = button;
                root->DispatchEvent(e);
            }
        };
        edge(s.mouseLeft,   s.prevLeft,   0);
        edge(s.mouseRight,  s.prevRight,  1);
        edge(s.mouseMiddle, s.prevMiddle, 2);

        if (s.wheelDelta.x != 0.f || s.wheelDelta.y != 0.f)
        {
            UIEvent e;
            e.type = UIEvent::Type::Wheel;
            e.mousePos = s.mousePos;
            e.wheel = s.wheelDelta;
            root->DispatchEvent(e);
        }

        if (Widget* hovered = root->HitTest(s.mousePos))
        {
            (void)hovered;
            wantsCapture = true;
        }
    }

    namespace {
        std::atomic<bool> g_wantsCaptureMouse{ false };
        std::atomic<bool> g_wantsCaptureKeyboard{ false };
    }

    bool UISystem::GlobalWantsCaptureMouse()
    { return g_wantsCaptureMouse.load(std::memory_order_relaxed); }
    bool UISystem::GlobalWantsCaptureKeyboard()
    { return g_wantsCaptureKeyboard.load(std::memory_order_relaxed); }

    void UISystem::SetFocused(WidgetHandle h)
    {
        if (Widget* old = WidgetRegistry::Get().Get(m_focused))
        {
            UIEvent e; e.type = UIEvent::Type::Blur;
            old->OnEvent(e);
        }
        m_focused = h;
        if (Widget* nu = WidgetRegistry::Get().Get(m_focused))
        {
            UIEvent e; e.type = UIEvent::Type::Focus;
            nu->OnEvent(e);
        }
    }

    void UISystem::DispatchKeyboard(UIInputState& input)
    {
        Widget* tgt = WidgetRegistry::Get().Get(m_focused);
        if (!tgt) { input.charsThisFrame.clear(); input.keysThisFrame.clear(); return; }

        if (!input.charsThisFrame.empty())
        {
            for (char c : input.charsThisFrame)
            {
                UIEvent e;
                e.type = UIEvent::Type::Char;
                e.character = static_cast<uint32_t>(static_cast<unsigned char>(c));
                e.shift = input.shift; e.ctrl = input.ctrl; e.alt = input.alt;
                tgt->OnEvent(e);
            }
        }
        for (const auto& k : input.keysThisFrame)
        {
            UIEvent e;
            e.type    = UIEvent::Type::Key;
            e.keyCode = k.code;
            e.shift = input.shift; e.ctrl = input.ctrl; e.alt = input.alt;
            e.keyDown = k.down;
            tgt->OnEvent(e);
        }
        input.charsThisFrame.clear();
        input.keysThisFrame.clear();
    }

    void UISystem::Tick(World& world, UIInputState& input,
                        const UICanvas& canvas, UIDrawList& drawList,
                        float dt)
    {
        m_wantsCaptureMouse = false;
        if (dt > 0.f) TweenSystem::Get().Tick(dt);

        // Build entries — UIRootComponent pool sorted by sortOrder.
        struct Entry { Entity e; UIRootComponent* root; };
        std::vector<Entry> entries;

        if (auto* pool = world.GetPool<UIRootComponent>())
        {
            const auto& ents = pool->Entities();
            auto&       data = pool->Data();
            entries.reserve(data.size());
            for (size_t i = 0; i < data.size(); ++i)
                entries.push_back({ ents[i], &data[i] });
            std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b)
                { return a.root->sortOrder < b.root->sortOrder; });
        }

        // Pointer events (top-most-first).
        Widget* clickTarget = nullptr;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        {
            if (!it->root->visible || !it->root->inputEnabled) continue;
            if (!it->root->root) continue;
            Widget* clickedHere = nullptr;
            DispatchPointerEvents(it->root->root.get(), input,
                                   m_wantsCaptureMouse, clickedHere);
            if (clickedHere && !clickTarget) clickTarget = clickedHere;
            if (m_wantsCaptureMouse) break;
        }
        if (clickTarget)
        {
            Widget* w = clickTarget;
            while (w && !w->Focusable()) w = w->GetParent();
            if (w) {
                WidgetHandle h = WidgetRegistry::Get().Acquire(w);
                if (h.id != m_focused.id) SetFocused(h);
            }
        }
        DispatchKeyboard(input);

        // ---- Widget-tree paint (screen-space) ------------------------------
        for (auto& en : entries)
        {
            if (!en.root->visible || !en.root->root) continue;
            const Vec2 cSize = (en.root->canvasSizeOverride.x > 0
                             && en.root->canvasSizeOverride.y > 0)
                             ? en.root->canvasSizeOverride : canvas.size;
            const Rect layoutRect{ Vec2{ 0, 0 }, cSize };
            en.root->root->LayoutAndPaint(layoutRect, drawList);
        }

        // ---- Flat-ECS UI primitives (screen-space) -------------------------
        // Position resolved via UIScreenSpaceComponent's anchor/offset/pivot.
        auto resolveScreenPos = [&](const UIScreenSpaceComponent& s,
                                    Vec2 visualSize, Vec2& outMin) {
            const Vec2 anchorPx{
                s.anchorX * canvas.size.x + s.offsetX,
                s.anchorY * canvas.size.y + s.offsetY,
            };
            outMin = {
                anchorPx.x - visualSize.x * s.pivotX,
                anchorPx.y - visualSize.y * s.pivotY,
            };
        };

        auto float4ToColor = [](const DirectX::XMFLOAT4& c) {
            return Color32(
                static_cast<uint8_t>(std::clamp(c.x, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.y, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.z, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.w, 0.f, 1.f) * 255.f));
        };

        // Image
        if (auto* imgPool = world.GetPool<UIImageComponent>())
        {
            const auto& ents = imgPool->Entities();
            auto&       data = imgPool->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                const UIImageComponent& img = data[i];
                if (!img.visible || img.srvGpuHandle == 0) continue;
                auto* ss = world.GetComponent<UIScreenSpaceComponent>(ents[i]);
                if (!ss) continue;
                const Vec2 visualSize{ img.sizeX, img.sizeY };
                Vec2 minPx{};
                resolveScreenPos(*ss, visualSize, minPx);
                UITextureRef tex; tex.srvGpuHandle = img.srvGpuHandle;
                drawList.AddImage(tex, minPx,
                                   { minPx.x + visualSize.x, minPx.y + visualSize.y },
                                   { img.uv0X, img.uv0Y },
                                   { img.uv1X, img.uv1Y },
                                   float4ToColor(img.tint));
            }
        }

        // Bar
        if (auto* barPool = world.GetPool<UIBarComponent>())
        {
            const auto& ents = barPool->Entities();
            auto&       data = barPool->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                const UIBarComponent& bar = data[i];
                if (!bar.visible) continue;
                auto* ss = world.GetComponent<UIScreenSpaceComponent>(ents[i]);
                if (!ss) continue;
                const Vec2 visualSize{ bar.sizeX, bar.sizeY };
                Vec2 minPx{};
                resolveScreenPos(*ss, visualSize, minPx);
                const Vec2 maxPx{ minPx.x + visualSize.x, minPx.y + visualSize.y };
                drawList.AddRectFilled(minPx, maxPx, float4ToColor(bar.backgroundColor));
                const float v = std::clamp(bar.value, 0.f, 1.f);
                if (v > 0.f)
                    drawList.AddRectFilled(minPx,
                                            { minPx.x + visualSize.x * v, maxPx.y },
                                            float4ToColor(bar.fillColor));
                if (bar.borderThick > 0.f)
                    drawList.AddRect(minPx, maxPx,
                                      float4ToColor(bar.borderColor), bar.borderThick);
            }
        }

        // Text
        if (auto* txtPool = world.GetPool<UITextComponent>())
        {
            const auto& ents = txtPool->Entities();
            auto&       data = txtPool->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                const UITextComponent& tc = data[i];
                if (!tc.visible || tc.text.empty()) continue;
                auto* ss = world.GetComponent<UIScreenSpaceComponent>(ents[i]);
                if (!ss) continue;

                const float oldScale = DefaultFont().Metrics().pixelScale;
                DefaultFont().Metrics().pixelScale = tc.scale;
                const Vec2 visualSize = DefaultFont().MeasureText(tc.text.c_str());
                Vec2 minPx{};
                resolveScreenPos(*ss, visualSize, minPx);
                DefaultFont().RenderText(drawList, minPx,
                                          float4ToColor(tc.color), tc.text.c_str());
                DefaultFont().Metrics().pixelScale = oldScale;
            }
        }

        // Publish capture flags.
        g_wantsCaptureMouse.store(m_wantsCaptureMouse, std::memory_order_relaxed);
        g_wantsCaptureKeyboard.store(
            WidgetRegistry::Get().Get(m_focused) != nullptr,
            std::memory_order_relaxed);

        input.prevLeft   = input.mouseLeft;
        input.prevRight  = input.mouseRight;
        input.prevMiddle = input.mouseMiddle;
        input.prevPos    = input.mousePos;
        input.wheelDelta = Vec2{ 0, 0 };
    }

} // namespace UI
