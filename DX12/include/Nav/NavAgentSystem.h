#pragma once

// NavAgentSystem — pathfinding + steering for entities carrying both
// NavAgentComponent and CharacterControllerComponent. Reads the tactical
// IntentComponent (`moveTarget` / `lookTarget`) as input.
//
// Pipeline:
//   IntentComponent.moveTarget
//     → NavAgentSystem queries NavMeshSystem.FindPath
//         → walks waypoints, computes steering vector
//             → writes CharacterControllerComponent.desiredHorizontalVelocity
//                 → KCC step (inside PhysicsSystem) does the actual sweep,
//                   ground snap, step up, slide-against-walls via Jolt
//                   CharacterVirtual.
//
// NavAgentSystem also writes LocalTransform.rotation directly (path-direction
// slerp, or Intent.lookTarget when useLookAt is set). KCC doesn't manage
// rotation — same convention PlayerControllerSystem uses.
//
// Runs in TickPhase::AI after the BT tick (which writes Intent) and BEFORE
// PhysicsStepSystem (which consumes desiredHorizontalVelocity in KCC).

class World;   // global ::World

namespace Nav
{
    class NavMeshSystem;

    // Tick every entity with NavAgentComponent. Reads sibling IntentComponent
    // for the move/look targets; entities with NavAgent but no Intent are
    // dormant (zero desired velocity, no path planning). No-op if NavMeshSystem
    // isn't ready.
    void NavAgentTick(::World& world, NavMeshSystem& nav, float dt);
}
