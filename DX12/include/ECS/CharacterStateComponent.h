#pragma once

// CharacterStateComponent — Lua-authored character FSM (IDLE / WALK / RUN /
// ATTACK / …). One entry per state; each entry pairs a name with the
// .ianim clip it should play. The component carries the runtime list so it
// survives save/load without the Lua Logic script having to re-register on
// every load.
//
// Lifecycle:
//   1. A Lua Logic script (or the SceneSerializer load path) calls
//      Character.AddState(entity, name, clipPath, {loop,speed}) one per
//      state, then Character.SetState(entity, name, 0) to enter the first
//      state.
//   2. CharacterStateSystem (runs BEFORE AnimationSystem each frame):
//        - Lazy-acquires each state's clipHandle the first time it sees the
//          state (via AnimationClipSystem::AcquireClip / BindToSkeleton).
//        - Ticks pendingIdx → currentIdx cross-fade by feeding the existing
//          AnimationComponent.{primary,secondary}Clip / blendWeight fields.
//   3. AnimationSystem reads those fields exactly as before — no change to
//      the sampler.
//
// BT integration: `Actions.SetState(ctx, { state="WALK", blend=0.25 })`
// just calls Character.SetState under the hood, so BTs can drive states
// without knowing the underlying component layout.

#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"   // Resource::AnimHandle
#include "Resource/SkeletonAsset.h"   // kInvalidClipIndex

#include <cstdint>
#include <string>
#include <vector>

struct CharacterStateComponent : ComponentBase
{
    struct StateEntry
    {
        std::string  name;                            // "IDLE", "WALK", ...
        std::string  clipPath;                        // "asset/anims/idle.ianim"
        Resource::AnimHandle clipResHandle{};         // AnimationClipSystem ref
        uint32_t     clipLibIdx     = kInvalidClipIndex; // ClipLibrary index after BindToSkeleton
        bool         looping        = true;
        float        playbackSpeed  = 1.0f;
    };

    std::vector<StateEntry> states;

    // -1 = no state selected yet (no clip playing).
    int32_t currentIdx = -1;
    // -1 = no cross-fade in progress. When set, the system ticks blendElapsed
    // → blendDuration and swaps secondary → primary on completion.
    int32_t pendingIdx = -1;

    // Active blend parameters.
    float blendDuration = 0.20f;     // total seconds for the in-flight transition
    float blendElapsed  = 0.0f;      // seconds since SetState was called

    // Used when SetState is called without an explicit blend arg.
    float defaultBlendDuration = 0.20f;
};
