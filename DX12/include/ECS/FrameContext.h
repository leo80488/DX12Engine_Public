#pragma once

// FrameContext — per-tick state handed to every ISystem::Update.
//
// Lives on the stack in EngineLoop::Tick() and is mutated in place as the
// loop crosses phase groups (e.g. physicsAlpha becomes meaningful only
// after the fixed-physics loop exits; isFixedTickPhase is true only during
// the catch-up loop). Systems read the fields they need and treat the
// rest as opaque.

#include <cstdint>

class CommandBuffer;
class TaskSystem;

struct FrameContext
{
    // ---- Timing -----------------------------------------------------------
    float    deltaTime        = 0.f;     // Real wall-clock dt (sec), unscaled.
                                         // Use for hit-stop timers, UI animation,
                                         // anything that must keep ticking even
                                         // when gameplay time is frozen.
    float    scaledDeltaTime  = 0.f;     // deltaTime * ScriptSystem::GetTimeScale().
                                         // Use for gameplay / physics / AI logic.
    float    fixedDeltaTime   = 1.f / 60.f;  // Fixed-physics step length (sec).
    float    physicsAlpha     = 0.f;     // [0,1] = accumulator / fixedDt.
                                         // Meaningful AFTER the fixed-physics loop.

    // ---- Frame indices ----------------------------------------------------
    uint64_t frameIndex       = 0;       // Wraps at 2^64; safe to use as cache key.
    uint64_t physicsStepIndex = 0;       // Increments once per Jolt step.

    // ---- Mode flags -------------------------------------------------------
    bool     isFixedTickPhase = false;   // True iff we are inside the accumulator
                                         // catch-up loop (FixedPhysicsPre/Step/Post).
                                         // Variable-rate systems should ignore the tick.
    bool     runUpdate        = true;    // Editor play-state gate. False when Stopped
                                         // or Paused (unless Step was requested).
                                         // Always-on systems (Camera, Transform,
                                         // Render) ignore this; gameplay/physics/AI
                                         // skip their Update when false.

    // ---- Per-frame entity references --------------------------------------
    // App maintains "the main camera" hint outside the scheduler (the entity
    // can disappear when a scene unloads) and republishes it here every tick
    // so systems don't have to re-resolve it. Stored as the raw Entity ID
    // (= uint32_t, NullEntity == 0) to keep this header dependency-free.
    uint32_t cameraEntity     = 0;

    // ---- Services ---------------------------------------------------------
    TaskSystem*    jobSystem     = nullptr;  // For Phase-internal parallelism (S3+).
    CommandBuffer* commandBuffer = nullptr;  // For deferred structural ECS changes.
};
