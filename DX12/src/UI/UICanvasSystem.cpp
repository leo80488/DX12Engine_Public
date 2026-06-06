#include "UI/UICanvasSystem.h"
#include "UI/UICanvas.h"
#include "UI/UIComponents.h"   // UIImageComponent (flat) — sprite anim also drives it
#include "UI/Font.h"
#include "ECS/ECS.h"
#include "ECS/GuidRegistry.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace UI
{
    namespace
    {
        Color32 ToColor32(const DirectX::XMFLOAT4& c)
        {
            return Color32(
                static_cast<uint8_t>(std::clamp(c.x, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.y, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.z, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.w, 0.f, 1.f) * 255.f));
        }

        DirectX::XMFLOAT4 MulF4(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b)
        {
            return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w };
        }

        // Unity RectTransform layout: parent rect + UIRect -> screen rect.
        // scale multiplies the pixel-denominated fields (size/offset) so the
        // whole subtree scales with the canvas scaler; anchors are normalised.
        Rect ComputeRect(const UIRect& r, const Rect& parent, float scale)
        {
            const Vec2 pmin = parent.mn;
            const Vec2 psize{ parent.mx.x - parent.mn.x, parent.mx.y - parent.mn.y };

            const Vec2 anchorLo{ pmin.x + r.anchorMin.x * psize.x,
                                 pmin.y + r.anchorMin.y * psize.y };
            const Vec2 anchorHi{ pmin.x + r.anchorMax.x * psize.x,
                                 pmin.y + r.anchorMax.y * psize.y };
            const Vec2 span{ anchorHi.x - anchorLo.x, anchorHi.y - anchorLo.y };

            const Vec2 size{ span.x + r.size.x * scale, span.y + r.size.y * scale };
            const Vec2 anchorRef{ anchorLo.x + r.pivot.x * span.x,
                                  anchorLo.y + r.pivot.y * span.y };
            const Vec2 pivotPt{ anchorRef.x + r.offset.x * scale,
                                anchorRef.y + r.offset.y * scale };

            Rect out;
            out.mn = { pivotPt.x - r.pivot.x * size.x, pivotPt.y - r.pivot.y * size.y };
            out.mx = { out.mn.x + size.x, out.mn.y + size.y };
            return out;
        }

        DirectX::XMFLOAT4 StateColor(const UIInteractable& it)
        {
            if (it.disabled) return it.disabledColor;
            if (it.pressed)  return it.pressedColor;
            if (it.hovered)  return it.hoverColor;
            return it.normalColor;
        }

        TextEffect MakeTextEffect(const UIText& t)
        {
            auto toC = [](const DirectX::XMFLOAT4& c) {
                return Color32(
                    static_cast<uint8_t>(std::clamp(c.x, 0.f, 1.f) * 255.f),
                    static_cast<uint8_t>(std::clamp(c.y, 0.f, 1.f) * 255.f),
                    static_cast<uint8_t>(std::clamp(c.z, 0.f, 1.f) * 255.f),
                    static_cast<uint8_t>(std::clamp(c.w, 0.f, 1.f) * 255.f));
            };
            using P = TextEffectPreset;
            const P p = t.effectPreset;
            TextEffect fx;
            fx.outline = (p == P::Outline || p == P::OutlineShadow);
            fx.glow    = (p == P::Glow    || p == P::GlowShadow);
            fx.shadow  = (p == P::Shadow  || p == P::OutlineShadow || p == P::GlowShadow);
            fx.jitter  = (p == P::Jitter);
            fx.outlineColor   = toC(t.outlineColor);
            fx.outlineWidthPx = t.outlineWidth;
            fx.glowColor      = toC(t.glowColor);
            fx.glowWidthPx    = t.glowWidth;
            fx.shadowColor    = toC(t.shadowColor);
            fx.shadowOffsetPx = { t.shadowOffsetX, t.shadowOffsetY };
            fx.jitterAmpPx    = t.jitterAmplitude;
            fx.jitterFreq     = t.jitterFrequency;
            return fx;
        }
    } // anonymous namespace

    void UICanvasSystem::Tick(World& world, UIInputState& input,
                              const UIScreen& screen, UIDrawList& drawList, float dt)
    {
        m_wantsCaptureMouse = false;
        if (dt > 0.f) m_uiTimeSec += dt;

        auto* canvasPool = world.GetPool<UICanvas>();
        if (!canvasPool || canvasPool->Size() == 0) return;

        const Vec2 screenSize = screen.size;
        if (screenSize.x <= 0.f || screenSize.y <= 0.f) return;

        // Invalidate last frame's computed rects so stale (now-detached) nodes
        // don't render or receive input.
        if (auto* crPool = world.GetPool<UIComputedRect>())
            for (auto& cr : crPool->Data()) cr.valid = false;

        // ---- 1. Resolve parent GUIDs + build child lists --------------------
        std::unordered_map<Entity, std::vector<Entity>> children;
        if (auto* parentPool = world.GetPool<UIParent>())
        {
            auto& ents = parentPool->Entities();
            auto& data = parentPool->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                UIParent& p = data[i];
                if ((p.parent == NullEntity || !world.IsAlive(p.parent)) && p.parentGuid.IsValid())
                {
                    const Entity resolved = ECS::GuidRegistry::Get().Find(p.parentGuid);
                    if (resolved != NullEntity) p.parent = resolved;
                }
                if (p.parent != NullEntity && world.IsAlive(p.parent))
                    children[p.parent].push_back(ents[i]);
            }
        }

        // ---- 2. Lay out each canvas subtree ---------------------------------
        struct CanvasEntry { Entity e; UICanvas* c; };
        std::vector<CanvasEntry> canvases;
        {
            auto& ents = canvasPool->Entities();
            auto& data = canvasPool->Data();
            canvases.reserve(data.size());
            for (size_t i = 0; i < data.size(); ++i) canvases.push_back({ ents[i], &data[i] });
            std::sort(canvases.begin(), canvases.end(),
                [](const CanvasEntry& a, const CanvasEntry& b) { return a.c->sortOrder < b.c->sortOrder; });
        }

        struct Drawable { Entity e; int sortKey; };
        std::vector<Drawable> drawables;
        std::vector<Entity>   raycastTargets;

        auto* crPool = world.EnsurePool<UIComputedRect>();
        // Guards against malformed UIParent cycles (would otherwise hang the DFS)
        // and double-processing an entity reachable from two canvases.
        std::unordered_set<Entity> visited;

        for (auto& ce : canvases)
        {
            UICanvas& cv = *ce.c;

            float scale = 1.f;
            if (cv.scaleMode == CanvasScaleMode::ScaleWithScreenSize)
            {
                const float refW = std::max(1.f, cv.referenceResolution.x);
                const float refH = std::max(1.f, cv.referenceResolution.y);
                const float logW = std::log2(std::max(1e-3f, screenSize.x / refW));
                const float logH = std::log2(std::max(1e-3f, screenSize.y / refH));
                const float m    = std::clamp(cv.matchWidthOrHeight, 0.f, 1.f);
                scale = std::exp2(logW * (1.f - m) + logH * m);
            }
            cv.computedScale = scale;

            // Canvas root rect = full screen.
            UIComputedRect rootCR;
            rootCR.rect    = Rect{ Vec2{ 0.f, 0.f }, screenSize };
            rootCR.depth   = 0;
            rootCR.sortKey = cv.sortOrder * 100000;
            rootCR.culled  = false;
            rootCR.valid   = true;
            crPool->Add(ce.e, rootCR);

            int orderCounter = 1;
            struct StackItem { Entity e; Rect parentRect; int depth; };
            std::vector<StackItem> stack;
            auto pushChildren = [&](Entity parent, const Rect& pr, int depth) {
                auto it = children.find(parent);
                if (it == children.end()) return;
                // Push reversed so first child is processed first (stable order).
                for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit)
                    stack.push_back({ *rit, pr, depth });
            };
            pushChildren(ce.e, rootCR.rect, 1);

            while (!stack.empty())
            {
                const StackItem si = stack.back();
                stack.pop_back();

                if (!visited.insert(si.e).second) continue; // cycle / re-entry guard

                const UIRect* r = world.GetComponent<UIRect>(si.e);
                if (!r) continue;

                const Rect cr = ComputeRect(*r, si.parentRect, scale);
                UIComputedRect cmp;
                cmp.rect    = cr;
                cmp.depth   = si.depth;
                cmp.sortKey = cv.sortOrder * 100000 + (orderCounter++);
                cmp.culled  = (cr.mx.x < 0.f || cr.mn.x > screenSize.x ||
                               cr.mx.y < 0.f || cr.mn.y > screenSize.y);
                cmp.valid   = true;
                crPool->Add(si.e, cmp);

                if (!cmp.culled)
                {
                    if (world.HasComponent<UIImage>(si.e) || world.HasComponent<UIText>(si.e))
                        drawables.push_back({ si.e, cmp.sortKey });
                    if (const auto* it = world.GetComponent<UIInteractable>(si.e); it && it->raycastTarget)
                        raycastTargets.push_back(si.e);
                }

                pushChildren(si.e, cr, si.depth + 1);
            }
        }

        // ---- 3. Interaction --------------------------------------------------
        // Top-most (highest sortKey) raycast target under the pointer.
        std::sort(raycastTargets.begin(), raycastTargets.end(),
            [&](Entity a, Entity b) {
                const auto* ca = world.GetComponent<UIComputedRect>(a);
                const auto* cb = world.GetComponent<UIComputedRect>(b);
                return (ca ? ca->sortKey : 0) > (cb ? cb->sortKey : 0);
            });

        const Vec2 mouse = input.mousePos;
        Entity hovered = NullEntity;
        for (Entity e : raycastTargets)
        {
            const auto* cr = world.GetComponent<UIComputedRect>(e);
            const auto* it = world.GetComponent<UIInteractable>(e);
            if (!cr || !it || it->disabled) continue;
            if (cr->rect.Contains(mouse)) { hovered = e; break; }
        }
        if (hovered != NullEntity) m_wantsCaptureMouse = true;

        const bool mouseHeld = input.mouseLeft;
        const bool mouseDown = input.mouseLeft && !input.prevLeft;
        const bool mouseUp   = !input.mouseLeft && input.prevLeft;

        if (auto* ip = world.GetPool<UIInteractable>())
        {
            auto& ents = ip->Entities();
            auto& data = ip->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                UIInteractable& it = data[i];
                const Entity e = ents[i];
                it.clicked = false;
                if (it.disabled) { it.hovered = false; it.pressed = false; continue; }
                it.hovered = (e == hovered);
                if (mouseDown && it.hovered) it.pressed = true;
                if (mouseUp)
                {
                    if (it.pressed && it.hovered) it.clicked = true; // release inside
                    it.pressed = false;
                }
                else if (!mouseHeld)
                {
                    it.pressed = false;
                }
            }
        }

        // ---- 4. Emit drawables (back-to-front) ------------------------------
        std::sort(drawables.begin(), drawables.end(),
            [](const Drawable& a, const Drawable& b) { return a.sortKey < b.sortKey; });

        const float oldScale = DefaultFont().Metrics().pixelScale;
        for (const auto& d : drawables)
        {
            const UIComputedRect* cr = world.GetComponent<UIComputedRect>(d.e);
            if (!cr || !cr->valid) continue;

            // Image fills the computed rect. srvGpuHandle 0 + empty path =
            // solid-colour panel (UIPass binds its 1x1 white texture). A path
            // that hasn't resolved yet is skipped to avoid a solid-colour flash.
            if (const auto* img = world.GetComponent<UIImage>(d.e); img && img->visible)
            {
                const bool waitingForTex = !img->texturePath.empty() && img->srvGpuHandle == 0;
                if (!waitingForTex)
                {
                    DirectX::XMFLOAT4 col = img->color;
                    if (const auto* it = world.GetComponent<UIInteractable>(d.e);
                        it && it->tintTransition)
                        col = MulF4(col, StateColor(*it));

                    UITextureRef tex; tex.srvGpuHandle = img->srvGpuHandle; // 0 -> white
                    drawList.AddImage(tex, cr->rect.mn, cr->rect.mx, img->uv0, img->uv1,
                                      ToColor32(col), UISamplerId(img->wrapMode, img->pointFilter));
                }
            }

            // Text aligned within the computed rect.
            if (const auto* txt = world.GetComponent<UIText>(d.e);
                txt && txt->visible && !txt->text.empty())
            {
                DefaultFont().Metrics().pixelScale = txt->fontScale;
                const Vec2 ts = DefaultFont().MeasureText(txt->text.c_str());
                const float rw = cr->rect.mx.x - cr->rect.mn.x;
                const float rh = cr->rect.mx.y - cr->rect.mn.y;

                float px = cr->rect.mn.x;
                if (txt->alignH == 1)      px = cr->rect.mn.x + (rw - ts.x) * 0.5f;
                else if (txt->alignH == 2) px = cr->rect.mx.x - ts.x;

                float py = cr->rect.mn.y;
                if (txt->alignV == 1)      py = cr->rect.mn.y + (rh - ts.y) * 0.5f;
                else if (txt->alignV == 2) py = cr->rect.mx.y - ts.y;

                const TextEffect fx = MakeTextEffect(*txt);
                DefaultFont().RenderTextStyled(drawList, Vec2{ px, py },
                                               ToColor32(txt->color), txt->text.c_str(),
                                               fx, m_uiTimeSec);
            }
        }
        DefaultFont().Metrics().pixelScale = oldScale;
    }

    // ---- Sprite-sheet animation --------------------------------------------
    void AdvanceSpriteAnimations(World& world, float dt)
    {
        auto* pool = world.GetPool<UISpriteAnimComponent>();
        if (!pool) return;

        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        for (size_t i = 0; i < data.size(); ++i)
        {
            UISpriteAnimComponent& a = data[i];
            const int cols  = a.columns > 0 ? a.columns : 1;
            const int rows  = a.rows    > 0 ? a.rows    : 1;
            int       total = a.frameCount > 0 ? a.frameCount : cols * rows;
            if (total < 1) total = 1;

            if (a.playing && a.fps > 0.f && total > 1)
            {
                a.elapsed += dt;
                const int step = static_cast<int>(a.elapsed * a.fps);
                if (a.pingpong)
                {
                    const int period = 2 * (total - 1);
                    const int p = step % period;
                    a.frame = (p < total) ? p : (period - p);
                }
                else if (a.loop)
                {
                    a.frame = step % total;
                }
                else
                {
                    a.frame = step < total ? step : (total - 1);
                }
            }
            a.frame = std::clamp(a.frame, 0, total - 1);

            // Current frame's grid sub-rect.
            const int   col = a.frame % cols;
            const int   row = a.frame / cols;
            const float u0  = static_cast<float>(col)     / static_cast<float>(cols);
            const float v0  = static_cast<float>(row)     / static_cast<float>(rows);
            const float u1  = static_cast<float>(col + 1) / static_cast<float>(cols);
            const float v1  = static_cast<float>(row + 1) / static_cast<float>(rows);

            const Entity e = ents[i];
            if (auto* img = world.GetComponent<UIImage>(e))
            {
                img->uv0 = { u0, v0 };
                img->uv1 = { u1, v1 };
            }
            if (auto* fimg = world.GetComponent<UIImageComponent>(e))
            {
                fimg->uv0X = u0; fimg->uv0Y = v0;
                fimg->uv1X = u1; fimg->uv1Y = v1;
            }
        }
    }

} // namespace UI
