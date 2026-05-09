#pragma once

// ECS components for audio sources and listeners.
// Spec: audio_system_architecture.md §3.1.
//
// Pure data — no XAudio2 / Windows headers leak through this header.
// AudioSystem reads/writes runtime fields (activeVoice, state); user code
// should treat those as opaque.

#include "Audio/AudioTypes.h"
#include "Resource/SystemHandles.h"   // AudioHandle alias
#include <string>

namespace Audio
{

// Attach to entities that should emit sound. is3D + transform → spatial; else
// the source plays as a 2D sample (no attenuation, panning, or doppler).
//
// `clipPath` is the editor / serialization-friendly identity (e.g. an
// asset-relative path "asset/footsteps/grass_01.wav"). On first frame after
// the path is set, AudioSystem calls AudioClipSystem::AcquireClip and stores
// the resulting handle in `clipHandle`; subsequent plays resolve straight
// off the handle without touching the path map.
struct AudioSourceComponent
{
    std::string           clipPath;                     // source-of-truth identity
    Resource::AudioHandle clipHandle = Resource::kInvalidAudioHandle;  // runtime cache
    BusType          bus            = BusType::SFX;
    float            volume         = 1.0f;
    float            pitch          = 1.0f;
    bool             loop           = false;
    bool             is3D           = true;
    bool             playOnEnable   = false;            // auto-play when component is added

    // 3D attenuation envelope (consulted when is3D == true).
    float            minDistance    = 1.0f;
    float            maxDistance    = 50.0f;
    AttenuationCurve curve          = AttenuationCurve::Linear;

    // ---- Runtime state, written by AudioSystem; user code should not edit ----
    VoiceHandle      activeVoice    = {};
    PlayState        state          = PlayState::Stopped;
};

// Attach to the entity whose transform represents the listener pose
// (typically the active camera). Only one component should be marked
// `active` at a time; if multiple are flagged, Audio3DSystem picks the
// first it encounters.
struct AudioListenerComponent
{
    float masterVolume = 1.0f;   // applied on top of BusType::Master volume
    bool  active       = true;
};

} // namespace Audio
