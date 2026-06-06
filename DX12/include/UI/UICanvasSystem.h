#pragma once

// UICanvasSystem — drives the entity-as-widget Canvas UI once per frame.
//
//   Tick(world, input, screen, drawList, dt):
//     1. Resolve UIParent GUIDs (deferred from scene load) + build child lists.
//     2. Per UICanvas (by sortOrder): compute scaler scale, lay out the subtree
//        (UIRect + parent UIComputedRect -> UIComputedRect, Unity RectTransform).
//     3. Interaction: raycast the pointer against UIInteractable targets
//        (top-most by sortKey) and update hover/press/click state.
//     4. Emit UIImage / UIText into the shared UIDrawList (back-to-front).
//
// Coexists with UISystem (widget tree + flat ECS) — both feed the same
// UIDrawList consumed by UIPass.

#include "UI/UISystem.h"   // UIInputState, UIScreen
#include "UI/UIDrawList.h"

class World;

namespace UI
{
    class UICanvasSystem
    {
    public:
        void Tick(World& world,
                  UIInputState&   input,
                  const UIScreen& screen,
                  UIDrawList&     drawList,
                  float           dt = 0.f);

        // True when the pointer is over a raycast-target interactable this
        // frame (so the caller can suppress world clicks). OR-combine with
        // UISystem::WantsCaptureMouse().
        bool WantsCaptureMouse() const { return m_wantsCaptureMouse; }

    private:
        bool  m_wantsCaptureMouse = false;
        float m_uiTimeSec = 0.f; // UI clock — drives text jitter
    };

} // namespace UI
