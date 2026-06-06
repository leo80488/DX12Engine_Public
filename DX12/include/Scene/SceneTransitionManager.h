#pragma once

// SceneTransitionManager — drives a fade-out -> loading-screen -> fade-in around
// an IGameMode scene switch, hiding the synchronous (blocking) scene load behind
// a full-screen black overlay with a progress bar + "loading" text.
//
// Why this lives OUTSIDE the GameModeStack (architecture note):
//   A scene switch in App::Run is an atomic Pop(old->Shutdown, which wipes the
//   old scene's entities) + Push(new->Init, which runs the blocking load) — see
//   App.cpp's mode-transition drain. A "LoadingScene" game mode therefore could
//   NOT fade the OLD scene out: by the time it became active the old scene is
//   already gone. So the transition must be driven by something that survives
//   the swap. That is this manager. The flow is:
//     1. FadeOut : old scene still active & rendering; black quad alpha 0 -> 1.
//     2. Loading : alpha = 1; trigger the (blocking) swap while fully black, so
//                  the unavoidable main-thread stall is masked by black.
//     3. FadeIn  : new scene now active & rendering; black quad alpha 1 -> 0.
//
//   The overlay is appended straight into the UIPass draw list (DrawOverlay),
//   NOT via ECS UI entities, so the new scene's world.Clear() can't wipe it
//   mid-fade.
//
// Phase A (current): the progress value is animated/heuristic. The blocking
// loader cannot report true 0..1 progress, so a real bar is impossible without
// making the load async. Phase B (future): make SceneInstanceLoader incremental
// and feed real readyCount/totalCount into the Loading phase — the state machine
// and overlay here stay unchanged.

#include <functional>
#include <memory>

class IGameMode;

namespace UI { class UIDrawList; }

class SceneTransitionManager
{
public:
    using ReplaceModeFn = std::function<void(std::unique_ptr<IGameMode>)>;

    SceneTransitionManager() = default;
    ~SceneTransitionManager(); // out-of-line: unique_ptr<IGameMode> needs a complete type

    // Begin a transition to @p target. Ignored if a transition is already in
    // flight (the running one keeps the screen) or @p target is null.
    void Begin(std::unique_ptr<IGameMode> target);

    // Advance the state machine by @p dt seconds. When fade-out completes it
    // calls @p replaceMode(target) to trigger the actual (blocking) mode swap —
    // App drains that request the SAME frame, so the stall happens while the
    // screen is already black. Safe (no-op) to call every frame when idle.
    void Tick(float dt, const ReplaceModeFn& replaceMode);

    // Append the full-screen fade quad + progress bar + text to @p dl. No-op
    // when idle. Call once per frame AFTER the normal UI tick and BEFORE the
    // UIPass consumes the draw list inside renderer.Render().
    void DrawOverlay(UI::UIDrawList& dl, unsigned int viewportW,
                     unsigned int viewportH) const;

    bool IsActive() const { return m_phase != Phase::Idle; }

    // ---- tunables (seconds) ----
    float fadeOutSeconds = 0.40f;
    float fadeInSeconds  = 0.55f;

private:
    enum class Phase { Idle, FadeOut, Loading, FadeIn };

    Phase                      m_phase         = Phase::Idle;
    std::unique_ptr<IGameMode> m_target;
    float                      m_elapsed       = 0.f; // within the current phase
    float                      m_alpha         = 0.f; // 0 = clear, 1 = full black
    float                      m_progress      = 0.f; // 0..1 bar fill
    bool                       m_swapRequested = false;
};
