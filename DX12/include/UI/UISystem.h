#pragma once

// UISystem — drives the UI pipeline once per frame.
//
//   Tick(world, input, drawList) =
//     1. Sort UIRoot entities by sortOrder.
//     2. For each visible root: dispatch synthesized UIEvents (mouse move,
//        button down/up, wheel) through Widget::DispatchEvent.
//     3. Run Widget::LayoutAndPaint, appending to the shared DrawList.
//
// The single drawList is later uploaded by UIPass; passing it in keeps this
// system decoupled from any specific renderer.

#include "UI/Widget.h"
#include "UI/UIDrawList.h"
#include "UI/WidgetRegistry.h"
#include <string>

class World;

namespace UI
{
    // Per-frame input state — populated by App from Mouse/Keyboard before Tick.
    struct UIInputState
    {
        Vec2  mousePos{ 0, 0 };       // canvas-space pixels (caller-translated)
        bool  mouseLeft   = false;
        bool  mouseRight  = false;
        bool  mouseMiddle = false;
        Vec2  wheelDelta{ 0, 0 };

        // Last-frame button state — UISystem diffs these to synthesize
        // MouseDown / MouseUp events (engine input layer reports current
        // state, not edge-triggered events).
        bool  prevLeft   = false;
        bool  prevRight  = false;
        bool  prevMiddle = false;
        Vec2  prevPos{ 0, 0 };

        // Drained text + key events. App fills these from Keyboard::ReadChar
        // and Keyboard::Readkey before each Tick; UISystem dispatches them
        // to the focused widget. UISystem clears them on return.
        std::string charsThisFrame;       // appended UTF-8 chars (engine reports ASCII)
        struct KeyEvt { uint32_t code; bool down; };
        std::vector<KeyEvt> keysThisFrame;
        bool shift = false, ctrl = false, alt = false;
    };

    struct UICanvas
    {
        Vec2 size{ 0, 0 }; // pixel dimensions of the rendering target
    };

    // World-space UI lives in `UI/WorldSpaceUI.h` + WorldSpaceUISystem +
    // WorldUIBillboardPass — completely separate path, no projection
    // matrix needed here.

    class UISystem
    {
    public:
        // Drive one frame of screen-space UI: events → layout → paint.
        // input.prev* is updated to current state on return so the caller
        // can pass the same struct again next frame.
        void Tick(World& world,
                  UIInputState& input,
                  const UICanvas& canvas,
                  UIDrawList& drawList,
                  float dt = 0.f);

        // Whether the most recent Tick consumed the cursor (cursor lay
        // inside a hit-testable widget). Game/Editor uses this to suppress
        // forwarding the click to the world (camera drag, picking, etc.).
        bool WantsCaptureMouse() const { return m_wantsCaptureMouse; }

        // Static accessor — `Window::SetWantCaptureMouse(...)` takes a free
        // function pointer (no captures). UISystem updates this each Tick
        // so the engine's input pipeline can poll it without holding a
        // reference to the UISystem instance.
        static bool GlobalWantsCaptureMouse();
        static bool GlobalWantsCaptureKeyboard();

        // Currently-focused widget (Char/Key events route here). Updated by
        // MouseDown on a focusable widget; explicit setter exposed for
        // gameplay code (e.g. opening a menu and forcing focus on a field).
        WidgetHandle Focused() const { return m_focused; }
        void         SetFocused(WidgetHandle h);

    private:
        bool         m_wantsCaptureMouse = false;
        WidgetHandle m_focused{};

        void DispatchKeyboard(UIInputState& input);
    };

} // namespace UI
