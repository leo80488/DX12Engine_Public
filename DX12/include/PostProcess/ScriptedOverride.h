#pragma once

// PostProcess::ScriptedOverrideSystem — transient, time-driven overrides
// that feed the ParameterBlender through the same Snapshot pipeline as
// spatial Volumes. Typical uses: damage flash, flashbang, cutscene filter,
// status-effect look.
//
// Lifecycle per entry:
//   Push(desc) returns a handle. The entry ticks forward each frame; its
//   weight is evaluated from the fadeIn / hold / fadeOut curve and fed to
//   the blender. Entry is auto-removed once elapsed >= totalDuration.
//   Pop(handle) cancels it early.
//
// This is independent of VolumeSystem — scripted overrides have no spatial
// component. The two producers are merged into a single snapshot list by
// PostProcess::Stack::Execute().

#include "PostProcess/Volume.h"          // VolumeOverride
#include "PostProcess/VolumeSystem.h"    // Snapshot

#include <cstdint>
#include <vector>

namespace PostProcess
{

using ScriptedOverrideHandle = uint32_t;
constexpr ScriptedOverrideHandle kInvalidScriptedOverrideHandle = 0xFFFFFFFFu;

// Piecewise-linear fade curve:
//   0 ─► 1 over fadeIn  ─► 1 for hold  ─► 1 ─► 0 over fadeOut.
// totalDuration is derived = fadeIn + hold + fadeOut.
struct ScriptedOverrideDesc
{
    VolumeOverride override;            // what to apply (which stages + values)
    int            priority = 1000;     // higher layers on top of spatial volumes

    float fadeIn  = 0.2f;               // seconds
    float hold    = 1.0f;               // seconds; use 0 for instant pulse
    float fadeOut = 0.5f;               // seconds

    // Optional label used by the Editor debug panel. Not functional.
    char  label[32] = {};
};

class ScriptedOverrideSystem
{
public:
    ScriptedOverrideSystem()  = default;
    ~ScriptedOverrideSystem() = default;

    ScriptedOverrideSystem(const ScriptedOverrideSystem&)            = delete;
    ScriptedOverrideSystem& operator=(const ScriptedOverrideSystem&) = delete;

    // Registers a transient override. The desc is copied; the returned
    // handle remains valid until the entry expires or Pop() is called.
    ScriptedOverrideHandle Push(const ScriptedOverrideDesc& desc);

    // Force-stop an override early. No-op for expired/invalid handles.
    void Pop(ScriptedOverrideHandle h);

    // Advances every live entry by @p dt, prunes expired entries, and
    // returns current-frame snapshots (sorted by priority ascending so the
    // blender applies lower priority first and higher layers on top).
    //
    // Returned Snapshot::override pointers are valid until the next call to
    // Tick() / Push() / Pop() — callers must consume the result within the
    // same frame, which is what PostProcess::Stack::Execute() does.
    std::vector<Snapshot> Tick(float dt);

    // Iteration for debug UI.
    struct LiveEntry
    {
        ScriptedOverrideHandle handle = kInvalidScriptedOverrideHandle;
        float elapsedSeconds = 0.0f;
        float totalDuration  = 0.0f;
        int   priority       = 0;
        const char* label    = "";
    };
    std::vector<LiveEntry> GetLiveEntries() const;

    size_t ActiveCount() const { return m_entries.size() - m_freeList.size(); }

private:
    struct Entry
    {
        bool  occupied = false;
        ScriptedOverrideDesc desc;
        float elapsedTime = 0.0f;
    };

    static float TotalDuration(const ScriptedOverrideDesc& d)
    {
        return d.fadeIn + d.hold + d.fadeOut;
    }

    // Piecewise-linear evaluator. elapsed is clamped to [0, totalDuration].
    static float EvaluateWeight(const ScriptedOverrideDesc& d, float elapsed);

    std::vector<Entry>    m_entries;
    std::vector<uint32_t> m_freeList;
};

} // namespace PostProcess
