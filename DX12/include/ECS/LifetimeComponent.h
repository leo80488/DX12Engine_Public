#pragma once

// LifetimeComponent — fixed-duration auto-destroy timer.
//
// Owned by runtime-spawned entities (VFX prefab instances, projectiles,
// short-lived debug markers) that should disappear after a known time.
// Persisting this to disk doesn't make sense — the timer would expire
// mid-save — so there is intentionally no ComponentSerializer for it. If
// an .iscn-loaded entity needs a lifetime, attach it after load instead.
//
// `LifetimeSystem` decrements `remaining` by frame dt every tick and calls
// `World::DestroyEntity` once the value reaches zero. Children of the
// entity are NOT auto-destroyed today — at most one level of attached
// follower is the canonical pattern (the spawning code wires
// FollowSocket / FollowEntity directly).

#include "ECS/ECS.h"
#include "ECS/ISystem.h"

struct LifetimeComponent
{
    float remaining = 1.f;   // seconds until destroy; <=0 triggers destroy this frame
    float total     = 1.f;   // initial duration, kept for inspector readouts / fades
};

// Runs early in the frame so a freshly-expired emitter doesn't pay the
// cost of going through TransformSystem / Particle update one more time.
// GameplayPreLogic also predates Animation / TimelineSystem, so a notify
// fired this frame can't immediately destroy the same-frame-spawned entity.
class LifetimeSystem final : public SystemInPhase<TickPhase::GameplayPreLogic>
{
public:
    const char* GetName() const override { return "LifetimeSystem"; }
    void Update(World& world, const FrameContext& ctx) override;
};
