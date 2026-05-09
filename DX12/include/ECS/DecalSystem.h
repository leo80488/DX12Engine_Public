#pragma once

// DecalLifetimeSystem — ticks lifetime countdown + fade-out for every
// DecalComponent in the world.
//
// Execution order (see decal_system_prompt.md §12.7):
//   DecalLifetimeSystem (this)
//     → (later) DecalCullSystem / DecalUploadSystem handled by Renderer
//
// The system:
//   1. Decrements DecalComponent::lifetime by dt for dynamic decals
//      (lifetime >= 0). Static decals (lifetime < 0) are skipped.
//   2. Drives fadeAlpha from 1 → 0 linearly across the last
//      fadeOutDuration seconds before expiry.
//   3. On expiry: destroys the entity (default) or just removes the
//      DecalComponent — caller chooses via DecalComponent::destroyEntityOnExpire.
//
// Iteration safety: entities/components are mutated only AFTER the
// component pool has been fully scanned (collected expired IDs first).

#include "ECS/ECS.h"

class DecalLifetimeSystem
{
public:
    // Call once per frame (Renderer already wires this into the per-frame
    // ECS system chain — see Renderer::BeginFrame).
    void Update(World& world, float dt);
};
