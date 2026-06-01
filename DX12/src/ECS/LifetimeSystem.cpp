#include "ECS/LifetimeComponent.h"

#include "ECS/FrameContext.h"

#include <vector>

void LifetimeSystem::Update(World& world, const FrameContext& ctx)
{
    // Variable-rate; use unscaled dt so a hit-stop (timeScale = 0) doesn't
    // stretch VFX-prefab cleanup indefinitely. Designers expect "2 second
    // duration" to mean 2 wall-clock seconds even when time is paused.
    const float dt = ctx.deltaTime;
    if (dt <= 0.f) return;

    // Two-phase: collect first, destroy second. Destroying mid-iteration
    // would invalidate the pool's swap-with-back during ForEach.
    std::vector<Entity> toDestroy;
    world.ForEach<LifetimeComponent>([&](Entity e, LifetimeComponent& lt)
    {
        lt.remaining -= dt;
        if (lt.remaining <= 0.f) toDestroy.push_back(e);
    });

    for (Entity e : toDestroy) world.DestroyEntity(e);
}
