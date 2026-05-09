#include "ECS/DecalSystem.h"
#include "ECS/Components.h"

#include <vector>

// ---------------------------------------------------------------------------
void DecalLifetimeSystem::Update(World& world, float dt)
{
    if (dt <= 0.0f) return;

    auto* pool = world.GetPool<DecalComponent>();
    if (!pool || pool->Size() == 0) return;

    // Snapshot the dense-vector bounds; we mutate DecalComponent fields in
    // place (safe under swap-with-back Erase semantics) and only collect
    // entity IDs for post-loop destroy / remove. That keeps the iteration
    // stable even if DestroyEntity reshuffles the pool.
    std::vector<Entity> toDestroy;
    std::vector<Entity> toRemoveComp;

    const auto& entities = pool->Entities();
    auto& data = pool->Data();

    for (size_t i = 0; i < entities.size(); ++i)
    {
        Entity e = entities[i];
        DecalComponent& dc = data[i];

        // Static decals: nothing to tick.
        if (dc.lifetime < 0.0f) continue;

        dc.lifetime -= dt;

        if (dc.lifetime <= 0.0f)
        {
            // Expired — queue for post-loop cleanup.
            if (dc.destroyEntityOnExpire) toDestroy.push_back(e);
            else                          toRemoveComp.push_back(e);
            continue;
        }

        // Linear fade-out across the last fadeOutDuration seconds. Guard the
        // divide so fadeOutDuration == 0 means "pop off at expiry, no fade".
        if (dc.fadeOutDuration > 0.0f && dc.lifetime < dc.fadeOutDuration)
            dc.fadeAlpha = dc.lifetime / dc.fadeOutDuration;
        else
            dc.fadeAlpha = 1.0f;
    }

    for (Entity e : toRemoveComp)
        world.RemoveComponent<DecalComponent>(e);
    for (Entity e : toDestroy)
        world.DestroyEntity(e);
}
