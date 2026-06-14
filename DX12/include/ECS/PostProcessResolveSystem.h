#pragma once

// PostProcessResolveSystem — resolves the post-process volume set + gameplay
// override stack into PostProcess::Runtime::resolved each frame.
//
// Phase: PreRender, registered AFTER the camera-stack resolve systems so it
// reads the final blended view position (avoids distance-falloff jitter during
// camera transitions — design §4.3). Pure read of ECS state + write of
// view-local runtime data, so it has no structural side effects.

#include "ECS/ISystem.h"
#include "ECS/TickPhase.h"

class World;
struct FrameContext;

class PostProcessResolveSystem : public SystemInPhase<TickPhase::PreRender>
{
public:
    const char* GetName() const override { return "PostProcessResolveSystem"; }
    void Update(World& world, const FrameContext& ctx) override;
};
