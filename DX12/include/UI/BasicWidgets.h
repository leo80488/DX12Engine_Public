#pragma once

// BasicWidgets — concrete Widget classes covering the design doc's §3.3
// minimum set: Text / Image / Button / Canvas / Flex / ProgressBar.
// One header keeps the include surface flat; if/when these grow they can be
// split per-file.

#include "UI/Widget.h"
#include <string>
#include <functional>

namespace UI
{
    // ---- Containers ----------------------------------------------------------

    // CanvasWidget — free / anchor layout. Children position themselves via
    // their own LayoutSpec (anchor + offset + pivot). HUD root is normally a
    // CanvasWidget the size of the viewport.
    class CanvasWidget : public Widget
    {
    public:
        Vec2 OnMeasure(Vec2 availableSize) override;
        // Default OnArrange (Widget::OnArrange) is fine — it hands the full
        // self-rect to each child, which then resolves its own anchor.
    };

    // FlexWidget — horizontal or vertical auto-layout (CSS flex-row/column).
    // Children are laid out along the main axis; cross-axis alignment is
    // configurable. Honours flexGrow on children for extra-space distribution.
    class FlexWidget : public Widget
    {
    public:
        FlexDirection direction = FlexDirection::Row;
        FlexJustify   justify   = FlexJustify::Start;
        FlexAlign     align     = FlexAlign::Start;
        float         spacing   = 0.f;
        Vec2          padding{ 0, 0 };

        Vec2 OnMeasure(Vec2 availableSize) override;
        void OnArrange(const LayoutContext& ctx) override;
    };

    // ---- Leaves --------------------------------------------------------------

    // ImageWidget — draws a single sprite/texture inside its rect.
    class ImageWidget : public Widget
    {
    public:
        UITextureRef texture;
        Vec2         uv0{ 0, 0 };
        Vec2         uv1{ 1, 1 };
        Color32      tint = Color32::White();

        void OnPaint(UIDrawList& dl) override;
    };

    // TextWidget — single-line text. Multi-line + word-wrap will arrive when
    // the font system gets metrics for them; for now a simple line block.
    class TextWidget : public Widget
    {
    public:
        std::string text;
        Color32     color = Color32::White();
        // Convenience setter — recomputes desired size from the bitmap font.
        void SetText(std::string t) { text = std::move(t); MarkVisualDirty(); MarkLayoutDirty(); }

        Vec2 OnMeasure(Vec2 availableSize) override;
        void OnPaint(UIDrawList& dl) override;
    };

    // ButtonWidget — interactive rect with hover/pressed states. Optional
    // label rendered through the bitmap font. onClick fires on mouse-up
    // inside the button area while the press began on it.
    class ButtonWidget : public Widget
    {
    public:
        std::string text;
        Color32     bgColor       = Color32(60, 60, 70, 255);
        Color32     bgColorHover  = Color32(80, 80, 100, 255);
        Color32     bgColorPress  = Color32(40, 40, 60, 255);
        Color32     borderColor   = Color32(120, 120, 140, 255);
        Color32     textColor     = Color32::White();
        float       borderThick   = 1.f;
        float       cornerRadius  = 4.f;

        std::function<void()> onClick;

        ButtonWidget() { hitTest = HitTestMode::Block; }

        Vec2 OnMeasure(Vec2 availableSize) override;
        void OnPaint(UIDrawList& dl) override;
        bool OnEvent(const UIEvent& e) override;

        bool IsHovered() const { return m_hovered; }
        bool IsPressed() const { return m_pressed; }

    private:
        bool m_hovered = false;
        bool m_pressed = false;
    };

    // ProgressBarWidget — horizontal fill, optional border. value in [0,1].
    class ProgressBarWidget : public Widget
    {
    public:
        float   value          = 0.5f;
        Color32 fillColor      = Color32(60, 200, 100, 255);
        Color32 backgroundColor = Color32(20, 20, 30, 255);
        Color32 borderColor    = Color32(120, 120, 140, 255);
        float   borderThick    = 1.f;

        void SetValue(float v) { value = v < 0 ? 0 : (v > 1 ? 1 : v); MarkVisualDirty(); }
        void OnPaint(UIDrawList& dl) override;
    };

    // InputFieldWidget — single-line text entry. ASCII-only via the engine
    // Keyboard's char queue (UTF-8 IME path is a Phase 5+ extension).
    // Emits onChange on every keystroke and onSubmit on Enter.
    class InputFieldWidget : public Widget
    {
    public:
        std::string text;
        std::string placeholder;
        Color32     bgColor          = Color32(20, 20, 30, 255);
        Color32     bgColorFocused   = Color32(30, 30, 50, 255);
        Color32     borderColor      = Color32(120, 120, 140, 255);
        Color32     borderColorFocus = Color32(80, 160, 240, 255);
        Color32     textColor        = Color32::White();
        Color32     placeholderColor = Color32(140, 140, 160, 255);
        Color32     caretColor       = Color32::White();
        float       borderThick      = 1.f;
        size_t      maxLen           = 256;

        std::function<void(const std::string&)> onChange;
        std::function<void(const std::string&)> onSubmit;

        InputFieldWidget() { hitTest = HitTestMode::Block; }
        bool Focusable() const override { return true; }

        Vec2 OnMeasure(Vec2 availableSize) override;
        void OnPaint(UIDrawList& dl) override;
        bool OnEvent(const UIEvent& e) override;

    private:
        bool m_focused = false;
        // Caret blink — driven by frame counter via UIPass; here we just keep
        // the pen-position cache between paints.
    };

} // namespace UI
