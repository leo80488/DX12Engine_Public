#pragma once

// PostProcess::IVolumeSource — abstraction over anything that produces
// per-frame override snapshots. PostProcess::Stack holds a list of sources
// and walks them each Execute() to build the blender's input.
//
// Current implementations:
//   - VolumeSystem         — standalone slot-based registry (editor/scripted)
//   - EntityVolumeSource   — reads ECS::VolumeComponent from World
//
// ScriptedOverrideSystem is *not* an IVolumeSource: it maintains mutable
// time state that must advance exactly once per frame, which is awkward to
// express through a stateless Gather() contract. Stack calls Tick() on it
// directly instead.

#include "PostProcess/Volume.h"              // Snapshot lives here
#include "PostProcess/IPostProcessEffect.h"  // Context

#include <vector>

namespace PostProcess
{

class IVolumeSource
{
public:
    virtual ~IVolumeSource() = default;

    // Appends current-frame snapshots to @p out. Snapshot::override pointers
    // must remain valid until the stack's blender finishes this frame.
    // Implementations read from @p ctx (cameraPos, world, deltaTime, ...)
    // but should not mutate it.
    virtual void Gather(const Context& ctx,
                        std::vector<Snapshot>& out) = 0;
};

} // namespace PostProcess
