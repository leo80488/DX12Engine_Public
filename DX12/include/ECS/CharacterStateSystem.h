#pragma once

// CharacterStateSystem — advances Lua-defined character state machines.
//
// Two responsibilities per tick:
//   1. Lazy-acquire clipLibIdx for every StateEntry that doesn't have one
//      yet (calls AnimationClipSystem::AcquireClip → BindToSkeleton).
//   2. Drive the cross-fade between currentIdx and pendingIdx by writing
//      the entity's AnimationComponent.{primary,secondary}Clip,
//      blendWeight, and primaryTime/secondaryTime. Once blendWeight ≥ 1,
//      swap secondary → primary and clear the pending state.
//
// Runs BEFORE AnimationSystem each frame so the sampler picks up freshly
// bound clip handles on the same frame the state was switched.

#include "ECS/ECS.h"

namespace Resource { class AnimationClipSystem; }
class SkeletonRegistry;

class CharacterStateSystem
{
public:
    explicit CharacterStateSystem(SkeletonRegistry& skeletons,
                                  Resource::AnimationClipSystem* clipSys)
        : m_skeletons(skeletons), m_clipSys(clipSys) {}

    // dt advances blendElapsed. clipLib is needed to register newly bound
    // clips into the global ClipLibrary so AnimationSystem can sample them.
    void Update(World& world, float dt, class ClipLibrary& clipLib);

    // Hot-swap if AnimationClipSystem rebinds (e.g., editor reload).
    void SetClipSystem(Resource::AnimationClipSystem* c) { m_clipSys = c; }

private:
    SkeletonRegistry&              m_skeletons;
    Resource::AnimationClipSystem* m_clipSys = nullptr;
};
