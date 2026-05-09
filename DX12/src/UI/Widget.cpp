#include "UI/Widget.h"
#include <algorithm>

namespace UI
{
    void Widget::RemoveChild(Widget* child)
    {
        auto it = std::find_if(m_children.begin(), m_children.end(),
            [child](const std::unique_ptr<Widget>& u) { return u.get() == child; });
        if (it != m_children.end())
        {
            (*it)->m_parent = nullptr;
            m_children.erase(it);
            MarkLayoutDirty();
        }
    }

    void Widget::ClearChildren()
    {
        for (auto& c : m_children) c->m_parent = nullptr;
        m_children.clear();
        MarkLayoutDirty();
    }

    void Widget::MarkLayoutDirty()
    {
        Widget* w = this;
        while (w && !w->m_layoutDirty)
        {
            w->m_layoutDirty = true;
            w->m_visualDirty = true;
            w = w->m_parent;
        }
    }

    Vec2 Widget::OnMeasure(Vec2 availableSize)
    {
        // Default: union children's desired sizes (used by simple containers).
        Vec2 d{ 0, 0 };
        for (auto& c : m_children)
        {
            if (!c->visible) continue;
            const Vec2 cd = c->OnMeasure(availableSize);
            d.x = std::max(d.x, cd.x);
            d.y = std::max(d.y, cd.y);
        }
        // Apply explicit size from spec when set.
        if (spec.size.x >= 0) d.x = spec.size.x;
        if (spec.size.y >= 0) d.y = spec.size.y;
        // Clamp to min/max.
        d.x = std::clamp(d.x, spec.minSize.x, spec.maxSize.x);
        d.y = std::clamp(d.y, spec.minSize.y, spec.maxSize.y);
        desiredSize = d;
        return d;
    }

    Rect Widget::ResolveSelfRect(const Rect& parentRect, Vec2 measured) const
    {
        const float pw = parentRect.Width();
        const float ph = parentRect.Height();

        // Stretch ignores measured size.
        if (spec.anchor == Anchor::Stretch)
        {
            const float l = parentRect.mn.x + spec.offset.x;
            const float t = parentRect.mn.y + spec.offset.y;
            const float r = parentRect.mx.x - (spec.size.x >= 0 ? spec.size.x : 0);
            const float b = parentRect.mx.y - (spec.size.y >= 0 ? spec.size.y : 0);
            return Rect{ Vec2{ l, t }, Vec2{ r, b } };
        }

        // Anchor reference point inside parent.
        Vec2 anchorPt;
        switch (spec.anchor)
        {
        case Anchor::TopLeft:     anchorPt = { parentRect.mn.x,                    parentRect.mn.y };                    break;
        case Anchor::Top:         anchorPt = { parentRect.mn.x + pw * 0.5f,        parentRect.mn.y };                    break;
        case Anchor::TopRight:    anchorPt = { parentRect.mx.x,                    parentRect.mn.y };                    break;
        case Anchor::Left:        anchorPt = { parentRect.mn.x,                    parentRect.mn.y + ph * 0.5f };        break;
        case Anchor::Center:      anchorPt = { parentRect.mn.x + pw * 0.5f,        parentRect.mn.y + ph * 0.5f };        break;
        case Anchor::Right:       anchorPt = { parentRect.mx.x,                    parentRect.mn.y + ph * 0.5f };        break;
        case Anchor::BottomLeft:  anchorPt = { parentRect.mn.x,                    parentRect.mx.y };                    break;
        case Anchor::Bottom:      anchorPt = { parentRect.mn.x + pw * 0.5f,        parentRect.mx.y };                    break;
        case Anchor::BottomRight: anchorPt = { parentRect.mx.x,                    parentRect.mx.y };                    break;
        default:                  anchorPt = { parentRect.mn.x,                    parentRect.mn.y };                    break;
        }

        const Vec2 origin{ anchorPt.x + spec.offset.x - measured.x * spec.pivot.x,
                           anchorPt.y + spec.offset.y - measured.y * spec.pivot.y };
        return Rect{ origin, Vec2{ origin.x + measured.x, origin.y + measured.y } };
    }

    void Widget::OnArrange(const LayoutContext& ctx)
    {
        rect = ResolveSelfRect(ctx.parentRect, desiredSize);
        // Default: hand the same rect to every child (overlay behaviour;
        // FlexWidget / GridWidget override).
        LayoutContext childCtx{ rect };
        for (auto& c : m_children)
        {
            if (!c->visible) continue;
            c->OnArrange(childCtx);
        }
        m_layoutDirty = false;
    }

    void Widget::PaintRecursive(UIDrawList& dl)
    {
        if (!visible) return;
        OnPaint(dl);
        for (auto& c : m_children) c->PaintRecursive(dl);
    }

    void Widget::LayoutAndPaint(const Rect& canvas, UIDrawList& dl)
    {
        if (!visible) return;
        // Always re-run measure/arrange — dirty short-circuits live inside
        // container widgets that cache children's desired sizes. The cost of
        // a single root traversal is negligible for typical UI tree sizes.
        OnMeasure(Vec2{ canvas.Width(), canvas.Height() });
        LayoutContext ctx{ canvas };
        OnArrange(ctx);
        PaintRecursive(dl);
    }

    Widget* Widget::HitTest(const Vec2& p)
    {
        if (!visible || hitTest == HitTestMode::Disabled) return nullptr;
        if (!rect.Contains(p)) return nullptr;
        // Traverse children top-most-first (last child = drawn on top).
        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
        {
            Widget* hit = (*it)->HitTest(p);
            if (hit) return hit;
        }
        return (hitTest == HitTestMode::Transparent) ? nullptr : this;
    }

    Widget* Widget::DispatchEvent(const UIEvent& e)
    {
        // Pointer events: resolve target via hit-test first, then bubble.
        if (e.type == UIEvent::Type::MouseMove ||
            e.type == UIEvent::Type::MouseDown ||
            e.type == UIEvent::Type::MouseUp   ||
            e.type == UIEvent::Type::Wheel)
        {
            Widget* target = HitTest(e.mousePos);
            while (target)
            {
                if (target->OnEvent(e)) return target;
                target = target->m_parent;
            }
            return nullptr;
        }

        // Non-pointer events: not routed here (focus drives them in Phase 3).
        if (OnEvent(e)) return this;
        return nullptr;
    }

} // namespace UI
