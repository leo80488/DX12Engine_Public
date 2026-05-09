#pragma once

// Events emitted by FollowSystem.
//
// Kept in a separate header (not FollowComponents.h) so gameplay code can
// include the event definitions without pulling in DirectXMath-heavy
// component layouts when all it wants is to Subscribe to a handler.

#include "ECS/ECS.h"

// FollowTargetLostEvent — first frame a follower's target handle becomes
// invalid (target destroyed, or generation mismatch after slot reuse).
// FollowSystem clears the follower's target to NullEntityHandle immediately
// after publishing so the event fires exactly once per lost target.
//
// Typical subscribers:
//   - equipment manager: re-holster / unequip the orphaned weapon
//   - VFX pool: recycle a particle emitter whose host died
//   - cleanup / lifetime scripts
struct FollowTargetLostEvent
{
    Entity       follower;        // entity that was following
    EntityHandle formerTarget;    // the now-stale handle it was chasing
};
