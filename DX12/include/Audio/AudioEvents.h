#pragma once

// Audio events — published through the engine's EventBus singleton.
// Spec: audio_system_architecture.md §4.
//
// AudioSystem subscribes to PlaySoundEvent / StopSoundEvent /
// SetAudioParamEvent and dispatches them into AudioEngine. UI / Lua /
// gameplay code only deals with these events — they never touch AudioEngine
// directly.

#include "Audio/AudioTypes.h"
#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <string>

namespace Audio
{

// Start playing a clip. If `entity` is NullEntity, the source is fire-and-
// forget (UI clicks, fully positional one-shots) — AudioSystem owns the
// resulting VoiceHandle and reclaims it when playback ends. If `entity`
// references a live entity with an AudioSourceComponent, the component's
// runtime state is updated instead.
struct PlaySoundEvent
{
    EntityHandle      entity     = NullEntityHandle;  // optional
    std::string       clipPath;                       // path to .wav (resolved by AudioEngine)
    BusType           bus        = BusType::SFX;
    float             volume     = 1.0f;
    float             pitch      = 1.0f;
    bool              loop       = false;
    bool              is3D       = false;
    // Position used for fire-and-forget 3D sounds when `entity` is null.
    DirectX::XMFLOAT3 position   = { 0.f, 0.f, 0.f };
    float             minDistance = 1.0f;
    float             maxDistance = 50.0f;
    AttenuationCurve  curve      = AttenuationCurve::Linear;
};

// Stop a currently-playing source. Either targets an entity (looks at its
// AudioSourceComponent) or a raw VoiceHandle for fire-and-forget plays.
struct StopSoundEvent
{
    EntityHandle entity      = NullEntityHandle;     // optional
    VoiceHandle  voice       = {};                   // optional — alternative to entity
    float        fadeOutSec  = 0.0f;                 // 0 = stop immediately
};

// Tweak runtime params on a live voice. Currently supports volume/pitch only.
struct SetAudioParamEvent
{
    enum Param : uint8_t { Volume, Pitch };

    EntityHandle entity = NullEntityHandle;
    VoiceHandle  voice  = {};
    Param        param  = Volume;
    float        value  = 1.0f;
};

// Mixer / settings → AudioEngine. Listened to by AudioEngine itself.
struct BusVolumeChangedEvent
{
    BusType bus    = BusType::Master;
    float   volume = 1.0f;
};

} // namespace Audio
