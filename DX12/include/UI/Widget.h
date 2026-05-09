#pragma once

// Widget — retained-mode UI node. The user-facing primitive of the UI layer.
//
// Hierarchy: a Widget owns its children (unique_ptr); parents borrow.
// Layout:    measure/arrange dual-pass with dirty propagation. Bottom-up
//            measure reports a desired size; top-down arrange assigns
//            absolute screen-space rects.
// Paint:     OnPaint receives a UIDrawList; the widget emits primitives.
//            Painting is purely additive — base Widget paints nothing.
// Event:     OnEvent receives a UIEvent; return true to consume.
//
// One Entity (UIRootComponent) owns one root Widget, NOT one widget per
// entity (see design doc §2.1). This is the standard AAA approach (UE
// Slate/UMG, Unity UGUI/UI Toolkit).

#include <memory>
#include <vector>
#include <cstdint>
#include "UI/UIDrawList.h"

namespace UI
{
    // ---- Layout primitives ---------------------------------------------------

    enum class Anchor : uint8_t
    {
        TopLeft,    Top,     TopRight,
        Left,       Center,  Right,
        BottomLeft, Bottom,  BottomRight,
        Stretch,    // fill parent in both axes (offset/size used as margin)
    };

    enum class FlexDirection : uint8_t { Row, Column };
    enum class FlexJustify   : uint8_t { Start, Center, End, SpaceBetween, SpaceAround };
    enum class FlexAlign     : uint8_t { Start, Center, End, Stretch };

    // What the widget asks for at measure time. Independent of where the
    // parent eventually places it. Negative size means "auto" (computed
    // from children / content during measure).
    struct LayoutSpec
    {
        Anchor   anchor = Anchor::TopLeft;
        Vec2     offset{ 0, 0 };       // position relative to anchor
        Vec2     size{ -1, -1 };       // -1 → auto / fill
        Vec2     pivot{ 0, 0 };        // origin within own rect (0..1)
        Vec2     minSize{ 0, 0 };
        Vec2     maxSize{ 1e6f, 1e6f };
        // Flex-only: child's grow factor along the main axis. 0 = fixed.
        float    flexGrow   = 0.f;
        float    flexShrink = 1.f;
    };

    struct LayoutContext
    {
        Rect parentRect;   // arrange target — absolute screen-space pixels.
    };

    // ---- Events --------------------------------------------------------------

    struct UIEvent
    {
        enum class Type : uint8_t
        {
            MouseMove,
            MouseDown,
            MouseUp,
            Wheel,
            Char,
            Key,
            Focus,
            Blur,
        };

        Type    type = Type::MouseMove;
        Vec2    mousePos{ 0, 0 };
        Vec2    wheel{ 0, 0 };
        uint8_t button = 0;        // 0=left, 1=right, 2=middle
        uint32_t character = 0;    // Type::Char
        uint32_t keyCode = 0;      // Type::Key (raw VK_*)
        bool    keyDown = false;   // Type::Key — true on press, false on release
        bool    shift = false, ctrl = false, alt = false;
    };

    enum class HitTestMode : uint8_t
    {
        Block,        // hit-testable, blocks events from reaching children behind
        Transparent,  // not hit-testable itself, but children still get hits
        Disabled,     // not hit-testable AND children skipped
    };

    // ---- Widget --------------------------------------------------------------

    class Widget
    {
    public:
        virtual ~Widget() = default;

        // Hierarchy ------------------------------------------------------------
        Widget* GetParent() const { return m_parent; }
        const std::vector<std::unique_ptr<Widget>>& Children() const { return m_children; }

        // AddChild gives the widget ownership of @p child and returns a raw
        // pointer for fluent API. Marks layout dirty up the chain.
        template <typename T>
        T* AddChild(std::unique_ptr<T> child)
        {
            T* raw = child.get();
            child->m_parent = this;
            m_children.push_back(std::move(child));
            MarkLayoutDirty();
            return raw;
        }

        void RemoveChild(Widget* child);
        void ClearChildren();

        // Layout ---------------------------------------------------------------
        LayoutSpec       spec;
        Rect             rect; // absolute screen-space pixels (set by Arrange)
        Vec2             desiredSize{ 0, 0 }; // produced by OnMeasure

        bool             visible = true;
        HitTestMode      hitTest = HitTestMode::Block;

        // Whether this widget can receive keyboard focus on click. Default
        // false — leaf widgets that consume text (input fields) flip it on.
        virtual bool     Focusable() const { return false; }

        // Override hooks -------------------------------------------------------
        // Bottom-up: report the size this widget wants given the parent's
        // available size (∞,∞ if unconstrained). Default measures children
        // and unions; container widgets override.
        virtual Vec2 OnMeasure(Vec2 availableSize);

        // Top-down: parent assigns @p ctx.parentRect; widget chooses its own
        // rect inside it (and lays out children). Default applies LayoutSpec
        // anchor + offset + size and hands the inner rect to children.
        virtual void OnArrange(const LayoutContext& ctx);

        // Append draw primitives. Called for the whole tree in pre-order.
        // Children are painted automatically — no need to recurse manually.
        virtual void OnPaint(UIDrawList& dl) { (void)dl; }

        // Receive an event. Return true to consume. Default: pass to children.
        virtual bool OnEvent(const UIEvent& e) { (void)e; return false; }

        // Dirty management -----------------------------------------------------
        void MarkLayoutDirty();
        void MarkVisualDirty() { m_visualDirty = true; }
        bool IsLayoutDirty() const { return m_layoutDirty; }

        // ---- Tree-level entry points (call on root only) --------------------
        // Performs measure → arrange → paint over the subtree rooted here.
        // canvas is the root's available rect (typically full viewport).
        void LayoutAndPaint(const Rect& canvas, UIDrawList& dl);

        // Drives event dispatch over the subtree. Returns the deepest widget
        // that consumed the event (or nullptr).
        Widget* DispatchEvent(const UIEvent& e);

        // Hit-test the subtree top-down. Returns the deepest hit widget.
        Widget* HitTest(const Vec2& p);

    protected:
        Widget*                                m_parent = nullptr;
        std::vector<std::unique_ptr<Widget>>   m_children;
        bool                                   m_layoutDirty = true;
        bool                                   m_visualDirty = true;

        // Helper: applies LayoutSpec to compute this widget's rect inside
        // @p parentRect. Available to subclass arrange overrides.
        Rect ResolveSelfRect(const Rect& parentRect, Vec2 measured) const;

        // Recursive paint helper — used by LayoutAndPaint. Walks the tree
        // honoring visibility.
        void PaintRecursive(UIDrawList& dl);
    };

} // namespace UI
