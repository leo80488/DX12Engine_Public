#pragma once

// PerceptionComponent — data-only sensing cache for AI. Populated by a
// PerceptionSystem (NOT YET IMPLEMENTED; this file ships the data layout so
// BT scripts and AITacticalSystem can read against a stable shape).
//
// Design intent per DesignMd/character_movement_architecture.md §2.3:
//
//   PerceptionSystem (raycast against BVH, navmesh raycast for nav-LOS)
//        ↓ writes
//   PerceptionComponent  ← this file
//        ↓ read by
//   BehaviorTreeSystem → AIIntentComponent (set Attack goal on threat seen)
//   AITacticalSystem  → NavAgent.destination (pick attack/cover position)
//
// Cadence: PerceptionSystem ticks at 10–30 Hz (visibility raycasts are
// expensive). BTs run at the same rate; per-frame is wasteful (§3.2).

#include "ECS/ECS.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

// One entity the AI can currently see, with auxiliary timing. Mirrors the
// "PerceivedEntity" struct in DesignMd §2.3.
struct PerceivedEntity
{
    Entity            entity        = NullEntity;
    DirectX::XMFLOAT3 lastKnownPos  = { 0.f, 0.f, 0.f };
    float             firstSeenTime = 0.f;   // when this target entered LOS this session
    float             lastSeenTime  = 0.f;   // for "I saw them N seconds ago" gates
};

// One sound the AI heard. Sounds decay over time — PerceptionSystem prunes
// entries past `audibleFor` seconds.
struct PerceivedSound
{
    DirectX::XMFLOAT3 origin       = { 0.f, 0.f, 0.f };
    float             loudness     = 0.f;   // gameplay-defined; higher = more attention
    float             heardAtTime  = 0.f;
    Entity            instigator   = NullEntity;  // who made the noise (NullEntity = unknown)
};

struct PerceptionComponent : ComponentBase
{
    // Things the AI currently sees. Cleared at the start of each
    // PerceptionSystem tick, then re-populated by view-cone + raycast.
    std::vector<PerceivedEntity> visibleEntities;

    // Sounds heard in the recent past (within audibleFor window). Aggregated
    // across PerceptionSystem ticks until they age out.
    std::vector<PerceivedSound>  heardSounds;

    // Sticky "last known threat" — survives losing line-of-sight so the AI
    // can continue investigating where the player disappeared to. Cleared
    // when investigation completes or after a long timeout.
    Entity            lastKnownThreat         = NullEntity;
    DirectX::XMFLOAT3 lastKnownThreatPosition = { 0.f, 0.f, 0.f };
    float             lastSeenTime            = 0.f;

    // Tunables — read by PerceptionSystem (not yet shipped).
    float             sightRange   = 15.f;
    float             sightConeDeg = 90.f;  // half-angle
    float             hearingRange = 12.f;
    float             audibleFor   = 8.f;   // sound pruning age
};
