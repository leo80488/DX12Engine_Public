#pragma once

// Video helpers — small utility functions that live OUTSIDE the per-tick
// VideoSystem so author code (Lua bindings, editor tools, test scenes) can
// call them at any time without locking the system loop.

#include "ECS/VideoComponent.h"

class IGraphicsDevice;

namespace Video
{
    // Defer-release every GPU resource the engine allocated for @p vc:
    //   * decoder + DPB heap (via backend->DestroyVideoDecoder)
    //   * each dpb[] NV12 texture (via gfx.DestroyTexture)
    // Safe to call from world-clear / entity-destroy hooks; uses the engine's
    // standard deferred-release queue so in-flight GPU work isn't disturbed.
    void ReleaseGPUResources(IGraphicsDevice& gfx, VideoComponent& vc);

    // Populate @p vc with a single NV12 colour-bar test pattern texture and
    // mark it as externally-driven (clears both source pointers, state=Paused).
    // The VideoPass picks it up the next render frame — verifies the entire
    // sampling / YUV→RGB chain works end-to-end without needing a real
    // bitstream / parser.
    //
    // Width must be even; height must be even (NV12 chroma is half-res).
    // Returns false on creation failure (out of VRAM, invalid dimensions).
    //
    // The pattern is the SMPTE 75 % colour bars laid out left-to-right:
    //   white, yellow, cyan, green, magenta, red, blue, black.
    bool LoadTestPattern(IGraphicsDevice& gfx, VideoComponent& vc,
                         uint32_t width, uint32_t height);

    // ----------------------------------------------------------------------
    // Playback controls — match the AnimationComponent convention (speed,
    // paused, looping). VideoSystem reads `state` + `playRate` each tick so
    // these helpers are just thin component mutators — call from gameplay
    // code, ImGui inspectors, or Lua bindings without going through a
    // signal/event bus.
    // ----------------------------------------------------------------------

    // Resume from `Stopped` (begin from PTS 0) or `Paused` (resume current
    // clock). When the component has neither a frameSource nor a decoded
    // dpb[] yet, Play still sets the state — VideoSystem will lazy-init the
    // decoder on the next tick.
    inline void Play(VideoComponent& vc)
    {
        if (vc.state == VideoPlaybackState::Stopped)
        {
            vc.clockSeconds   = 0.0;
            vc.lastDecodedPts = -1.0;
        }
        vc.state = VideoPlaybackState::Playing;
    }

    // Freeze playback; clock + dpb stay where they are so renderers keep
    // sampling the last decoded frame.
    inline void Pause(VideoComponent& vc)
    {
        if (vc.state == VideoPlaybackState::Playing)
            vc.state = VideoPlaybackState::Paused;
    }

    // Toggle Play ↔ Pause. Stopped → Play (Resume-from-start).
    inline void TogglePause(VideoComponent& vc)
    {
        switch (vc.state)
        {
        case VideoPlaybackState::Playing: vc.state = VideoPlaybackState::Paused; break;
        case VideoPlaybackState::Paused:  vc.state = VideoPlaybackState::Playing; break;
        case VideoPlaybackState::Stopped: Play(vc); break;
        }
    }

    // Halt playback; clock returns to 0 and lastDecodedPts is invalidated so
    // a subsequent Play starts decoding from the beginning. Decoded dpb[]
    // textures stay allocated (cheap) — VideoQuadPass/VideoPass freezes on
    // whatever was last shown until Stop resets currentDpbSlot too.
    inline void Stop(VideoComponent& vc)
    {
        vc.state          = VideoPlaybackState::Stopped;
        vc.clockSeconds   = 0.0;
        vc.lastDecodedPts = -1.0;
    }

    // Stop + Play in one call.
    inline void Restart(VideoComponent& vc)
    {
        Stop(vc);
        Play(vc);
    }

    // Set the playback rate multiplier (0 freezes, negative reverses if the
    // frameSource supports it — most do NOT, so the result is implementation-
    // defined; safe range is [0, +∞)). Matches AnimationComponent::speed.
    inline void SetSpeed(VideoComponent& vc, float speedMul)
    {
        vc.playRate = speedMul;
    }

    // Seek to PTS @p timeSeconds. Default impl jumps the clock — actual frame
    // delivery depends on the frameSource: a real video source may need to
    // seek to the nearest IDR / keyframe internally before serving the
    // requested time. The test-pattern path (no source) just advances the
    // clock; the displayed frame stays the same.
    void Seek(VideoComponent& vc, double timeSeconds);
} // namespace Video
