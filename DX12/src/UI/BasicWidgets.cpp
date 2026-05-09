#include "UI/BasicWidgets.h"
#include "UI/Font.h"
#include <algorithm>

namespace UI
{
    // ===== Containers =========================================================

    Vec2 CanvasWidget::OnMeasure(Vec2 availableSize)
    {
        // Measure children (so they cache their desired sizes for arrange),
        // but the canvas itself is sized to the parent's available area.
        for (auto& c : m_children)
        {
            if (!c->visible) continue;
            c->OnMeasure(availableSize);
        }
        if (spec.size.x >= 0) availableSize.x = spec.size.x;
        if (spec.size.y >= 0) availableSize.y = spec.size.y;
        desiredSize = availableSize;
        return desiredSize;
    }

    // ---- Flex ----------------------------------------------------------------

    Vec2 FlexWidget::OnMeasure(Vec2 availableSize)
    {
        // Subtract padding from the available area children may consume.
        const Vec2 inner{
            std::max(0.f, availableSize.x - 2.f * padding.x),
            std::max(0.f, availableSize.y - 2.f * padding.y),
        };

        float main = 0.f;
        float cross = 0.f;
        int   visibleCount = 0;
        for (auto& c : m_children)
        {
            if (!c->visible) continue;
            const Vec2 cd = c->OnMeasure(inner);
            if (direction == FlexDirection::Row)
            {
                main += cd.x;
                cross = std::max(cross, cd.y);
            }
            else
            {
                main += cd.y;
                cross = std::max(cross, cd.x);
            }
            ++visibleCount;
        }
        if (visibleCount > 1)
            main += spacing * static_cast<float>(visibleCount - 1);

        Vec2 d = (direction == FlexDirection::Row)
            ? Vec2{ main + 2 * padding.x, cross + 2 * padding.y }
            : Vec2{ cross + 2 * padding.x, main + 2 * padding.y };

        if (spec.size.x >= 0) d.x = spec.size.x;
        if (spec.size.y >= 0) d.y = spec.size.y;
        d.x = std::clamp(d.x, spec.minSize.x, spec.maxSize.x);
        d.y = std::clamp(d.y, spec.minSize.y, spec.maxSize.y);
        desiredSize = d;
        return d;
    }

    void FlexWidget::OnArrange(const LayoutContext& ctx)
    {
        // Resolve own rect inside parent.
        rect = ResolveSelfRect(ctx.parentRect, desiredSize);

        const float innerL = rect.mn.x + padding.x;
        const float innerT = rect.mn.y + padding.y;
        const float innerR = rect.mx.x - padding.x;
        const float innerB = rect.mx.y - padding.y;
        const float innerW = std::max(0.f, innerR - innerL);
        const float innerH = std::max(0.f, innerB - innerT);

        // Visible children list + measured main extents.
        struct Item { Widget* w; float main; float cross; };
        std::vector<Item> items;
        items.reserve(m_children.size());
        float totalMain  = 0.f;
        float totalGrow  = 0.f;
        for (auto& c : m_children)
        {
            if (!c->visible) continue;
            const float m = (direction == FlexDirection::Row) ? c->desiredSize.x : c->desiredSize.y;
            const float x = (direction == FlexDirection::Row) ? c->desiredSize.y : c->desiredSize.x;
            items.push_back({ c.get(), m, x });
            totalMain += m;
            totalGrow += c->spec.flexGrow;
        }
        const float spacingTotal = spacing * static_cast<float>(items.empty() ? 0 : items.size() - 1);
        float free = ((direction == FlexDirection::Row) ? innerW : innerH) - totalMain - spacingTotal;

        // Distribute extra space if any item wants to grow.
        if (free > 0.f && totalGrow > 0.f)
        {
            for (auto& it : items)
                it.main += free * (it.w->spec.flexGrow / totalGrow);
            free = 0.f;
        }

        // Justify on main axis.
        const int n = static_cast<int>(items.size());
        float startMain = 0.f;
        float gap = spacing;
        switch (justify)
        {
        case FlexJustify::Start:        startMain = 0.f; break;
        case FlexJustify::Center:       startMain = free * 0.5f; break;
        case FlexJustify::End:          startMain = free; break;
        case FlexJustify::SpaceBetween: if (n > 1) gap += free / static_cast<float>(n - 1); break;
        case FlexJustify::SpaceAround:  if (n > 0) { const float s = free / static_cast<float>(n); gap += s; startMain = s * 0.5f; } break;
        }

        // Place each child.
        float cursor = startMain;
        for (auto& it : items)
        {
            // Cross-axis alignment.
            float crossSize = (align == FlexAlign::Stretch)
                ? ((direction == FlexDirection::Row) ? innerH : innerW)
                : it.cross;
            float crossOff = 0.f;
            const float crossSpace = (direction == FlexDirection::Row) ? innerH : innerW;
            switch (align)
            {
            case FlexAlign::Start:   crossOff = 0.f; break;
            case FlexAlign::Center:  crossOff = (crossSpace - crossSize) * 0.5f; break;
            case FlexAlign::End:     crossOff = crossSpace - crossSize; break;
            case FlexAlign::Stretch: crossOff = 0.f; break;
            }

            Rect r;
            if (direction == FlexDirection::Row)
            {
                r.mn = { innerL + cursor, innerT + crossOff };
                r.mx = { r.mn.x + it.main, r.mn.y + crossSize };
            }
            else
            {
                r.mn = { innerL + crossOff, innerT + cursor };
                r.mx = { r.mn.x + crossSize, r.mn.y + it.main };
            }

            // Hand the child its allocated rect via a trivial parent rect.
            it.w->desiredSize = { r.Width(), r.Height() };
            // Override anchor/offset/pivot so the child lands exactly where
            // FlexWidget computed.
            const LayoutSpec savedSpec = it.w->spec;
            it.w->spec.anchor = Anchor::TopLeft;
            it.w->spec.offset = { 0, 0 };
            it.w->spec.pivot  = { 0, 0 };
            it.w->spec.size   = { r.Width(), r.Height() };
            LayoutContext childCtx{ r };
            it.w->OnArrange(childCtx);
            it.w->spec = savedSpec;

            cursor += it.main + gap;
        }
        m_layoutDirty = false;
    }

    // ===== Leaves =============================================================

    void ImageWidget::OnPaint(UIDrawList& dl)
    {
        if (texture.srvGpuHandle == 0) return;
        dl.AddImage(texture, rect.mn, rect.mx, uv0, uv1, tint);
    }

    Vec2 TextWidget::OnMeasure(Vec2 /*available*/)
    {
        const Vec2 d = DefaultFont().MeasureText(text.c_str());
        Vec2 out = d;
        if (spec.size.x >= 0) out.x = spec.size.x;
        if (spec.size.y >= 0) out.y = spec.size.y;
        out.x = std::clamp(out.x, spec.minSize.x, spec.maxSize.x);
        out.y = std::clamp(out.y, spec.minSize.y, spec.maxSize.y);
        desiredSize = out;
        return out;
    }

    void TextWidget::OnPaint(UIDrawList& dl)
    {
        DefaultFont().RenderText(dl, rect.mn, color, text.c_str());
    }

    // ---- Button --------------------------------------------------------------

    Vec2 ButtonWidget::OnMeasure(Vec2 available)
    {
        // Default sizing: text size + 2× a comfortable padding (8px each axis).
        Vec2 d = DefaultFont().MeasureText(text.c_str());
        d.x += 16.f;
        d.y += 12.f;
        if (spec.size.x >= 0) d.x = spec.size.x;
        if (spec.size.y >= 0) d.y = spec.size.y;
        d.x = std::clamp(d.x, spec.minSize.x, spec.maxSize.x);
        d.y = std::clamp(d.y, spec.minSize.y, spec.maxSize.y);
        desiredSize = d;
        (void)available;
        return d;
    }

    void ButtonWidget::OnPaint(UIDrawList& dl)
    {
        const Color32 bg = m_pressed ? bgColorPress
                          : m_hovered ? bgColorHover
                                      : bgColor;
        dl.AddRectFilledRounded(rect.mn, rect.mx, bg, cornerRadius);
        if (borderThick > 0.f)
            dl.AddRect(rect.mn, rect.mx, borderColor, borderThick);

        if (!text.empty())
        {
            const Vec2 ts = DefaultFont().MeasureText(text.c_str());
            const Vec2 pos{
                rect.mn.x + (rect.Width()  - ts.x) * 0.5f,
                rect.mn.y + (rect.Height() - ts.y) * 0.5f,
            };
            DefaultFont().RenderText(dl, pos, textColor, text.c_str());
        }
    }

    bool ButtonWidget::OnEvent(const UIEvent& e)
    {
        switch (e.type)
        {
        case UIEvent::Type::MouseMove:
            m_hovered = rect.Contains(e.mousePos);
            return false; // hover doesn't consume — let other widgets see motion
        case UIEvent::Type::MouseDown:
            if (e.button == 0 && rect.Contains(e.mousePos))
            { m_pressed = true; return true; }
            return false;
        case UIEvent::Type::MouseUp:
            if (e.button == 0 && m_pressed)
            {
                const bool inside = rect.Contains(e.mousePos);
                m_pressed = false;
                if (inside && onClick) onClick();
                return inside;
            }
            return false;
        default:
            return false;
        }
    }

    // ---- InputField ----------------------------------------------------------

    Vec2 InputFieldWidget::OnMeasure(Vec2 /*available*/)
    {
        // Default: room for placeholder/text + 12px vertical padding.
        const Vec2 ts = DefaultFont().MeasureText(
            text.empty() ? placeholder.c_str() : text.c_str());
        Vec2 d{ std::max(120.f, ts.x + 16.f), ts.y + 12.f };
        if (spec.size.x >= 0) d.x = spec.size.x;
        if (spec.size.y >= 0) d.y = spec.size.y;
        d.x = std::clamp(d.x, spec.minSize.x, spec.maxSize.x);
        d.y = std::clamp(d.y, spec.minSize.y, spec.maxSize.y);
        desiredSize = d;
        return d;
    }

    void InputFieldWidget::OnPaint(UIDrawList& dl)
    {
        const Color32 bg = m_focused ? bgColorFocused   : bgColor;
        const Color32 bd = m_focused ? borderColorFocus : borderColor;
        dl.AddRectFilled(rect.mn, rect.mx, bg);
        if (borderThick > 0.f) dl.AddRect(rect.mn, rect.mx, bd, borderThick);

        const Vec2 textPos{ rect.mn.x + 8.f,
                             rect.mn.y + (rect.Height() - DefaultFont().Metrics().lineHeight
                                                          * DefaultFont().Metrics().pixelScale) * 0.5f };
        if (!text.empty())
        {
            DefaultFont().RenderText(dl, textPos, textColor, text.c_str());
        }
        else if (!placeholder.empty())
        {
            DefaultFont().RenderText(dl, textPos, placeholderColor, placeholder.c_str());
        }

        if (m_focused)
        {
            // Caret right after the last glyph — measure current text to find x.
            const Vec2 ts = text.empty() ? Vec2{ 0, 0 }
                                         : DefaultFont().MeasureText(text.c_str());
            const float cx = textPos.x + ts.x;
            const float topY = textPos.y;
            const float botY = topY + DefaultFont().Metrics().lineHeight
                                       * DefaultFont().Metrics().pixelScale;
            dl.AddRectFilled({ cx, topY }, { cx + 1.5f, botY }, caretColor);
        }
    }

    bool InputFieldWidget::OnEvent(const UIEvent& e)
    {
        switch (e.type)
        {
        case UIEvent::Type::Focus: m_focused = true;  MarkVisualDirty(); return true;
        case UIEvent::Type::Blur:  m_focused = false; MarkVisualDirty(); return true;

        case UIEvent::Type::Char:
        {
            const auto c = static_cast<char>(e.character);
            if (c == 8) // backspace
            {
                if (!text.empty()) { text.pop_back(); if (onChange) onChange(text); MarkVisualDirty(); }
                return true;
            }
            if (c == '\r' || c == '\n')
            {
                if (onSubmit) onSubmit(text);
                return true;
            }
            if (c >= 32 && c < 127 && text.size() < maxLen)
            {
                text.push_back(c);
                if (onChange) onChange(text);
                MarkVisualDirty();
            }
            return true;
        }

        case UIEvent::Type::Key:
            // Reserved for arrow keys / clipboard etc; v1 ignores most.
            return true;

        case UIEvent::Type::MouseDown:
            return rect.Contains(e.mousePos); // capture so click-to-focus works
        default:
            return false;
        }
    }

    // ---- ProgressBar ---------------------------------------------------------

    void ProgressBarWidget::OnPaint(UIDrawList& dl)
    {
        dl.AddRectFilled(rect.mn, rect.mx, backgroundColor);
        const float w = std::max(0.f, rect.Width()) * std::clamp(value, 0.f, 1.f);
        if (w > 0.f)
        {
            dl.AddRectFilled(rect.mn,
                             { rect.mn.x + w, rect.mx.y },
                             fillColor);
        }
        if (borderThick > 0.f)
            dl.AddRect(rect.mn, rect.mx, borderColor, borderThick);
    }

} // namespace UI
