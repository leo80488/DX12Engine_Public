#pragma once

// PostProcess::Runtime — process-global home for the post-process volume
// system's per-frame state, plus the non-spatial gameplay override stack.
//
// Why a global (vs. an ECS singleton component): the producer is an ECS system
// (PostProcessResolveSystem) but the CONSUMER is the Renderer, which lives
// outside the ECS. The engine already exposes cross-cutting state through Get()
// singletons (AssetFS, GuidRegistry, ProfileSystem), so this matches the house
// style and lets the editor / Lua / renderer all reach the same state.
//
//   PostProcessResolveSystem  ── writes ──►  Runtime::resolved  ── read ──► Renderer
//   Lua / editor              ── push/pop ─►  Runtime::overrides ── read ──► resolve
//
// §6 of DesignMd/PostProcessVolume_Architecture.md: the gameplay override stack
// is a separate, non-spatial layer applied on top of the volume blend, each
// entry a profile + time envelope, typically at higher priority than volumes.

#include "PostProcess/PostProcessProfile.h"
#include "PostProcess/ResolvedPostProcessSettings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace PostProcess
{

// One transient, event-driven override (damage flash, skill filter, low-HP
// desaturate, cutscene grade...). Holds an inline profile (Overridable, only
// the touched properties) and a fadeIn→hold→fadeOut time envelope.
struct PostProcessOverride
{
    uint64_t           id           = 0;        // assigned by PushOverride
    float              priority     = 1000.0f;  // usually > any volume
    float              masterWeight = 1.0f;     // 0..1
    PostProcessProfile profile;                 // inline per-property overrides

    // Time envelope (seconds). hold < 0 means "stay at full until removed".
    float    elapsed = 0.0f;
    float    fadeIn  = 0.2f;
    float    hold    = 1.0f;
    float    fadeOut = 0.5f;

    uint32_t layerMask = 0xFFFFFFFFu;
    char     label[32] = {};
};

// Evaluate the 0..1 envelope weight for an override at its current elapsed time.
float EvaluateOverrideEnvelope(const PostProcessOverride& o);

// True once an override has fully played out (only when hold >= 0).
bool IsOverrideExpired(const PostProcessOverride& o);

// Per-volume debug snapshot, filled during resolve when debugCapture is on so
// the editor overlay can show "which volumes is the camera in and at what
// weight".
struct VolumeHitDebug
{
    char     label[64] = {};
    float    weight    = 0.0f;
    float    priority  = 0.0f;
    bool     isGlobal  = false;
};

class Runtime
{
public:
    static Runtime& Get();

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Latest resolved settings for the main view — written each frame by the
    // resolve system, read by the Renderer when filling PostProcess::Context.
    ResolvedPostProcessSettings resolved;

    // Gameplay override stack (non-spatial). Mutated by Lua/editor, ticked &
    // applied by the resolve system.
    std::vector<PostProcessOverride> overrides;

    // --- Override stack ops -------------------------------------------------
    // Push a copy; assigns and returns a fresh id (also stamped into the copy).
    uint64_t PushOverride(PostProcessOverride ov);
    void     RemoveOverride(uint64_t id);
    void     ClearOverrides() { overrides.clear(); }

    // --- Debug overlay ------------------------------------------------------
    bool                        debugCapture = false;  // editor toggles this
    std::vector<VolumeHitDebug> debugHits;             // filled when debugCapture

private:
    Runtime() = default;
    uint64_t m_nextId = 1;
};

} // namespace PostProcess
