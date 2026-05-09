#pragma once

// Audio system shared types — small POD structs used by every layer:
// engine wrapper, ECS components, events, systems.
//
// Mirrors audio_system_architecture.md §2 (VoiceHandle, BusType, PlayParams).

#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

namespace Audio
{

// Submix bus categories — one IXAudio2SubmixVoice per bus, all routed to the
// mastering voice. Mixer/UI volume sliders set per-bus volume independently.
enum class BusType : uint32_t
{
    Master = 0,
    Music,
    SFX,
    Voice,
    Ambient,
    UI,
    Count
};

// Lifecycle of a source as observed from the ECS layer. The audio worker
// thread can flip Playing → Stopped via an atomic on the voice record; the
// AudioSystem syncs that into the component once per frame.
enum class PlayState : uint8_t
{
    Stopped = 0,
    Playing,
    Paused,
    Stopping,   // fading out, will become Stopped when fade completes
};

// 3D distance attenuation curve. Linear is the only one wired up in v1; the
// other entries are placeholders for future X3DAudio custom-curve support.
enum class AttenuationCurve : uint8_t
{
    Linear = 0,
    Logarithmic,
    InverseSquare,
};

// VoiceHandle — opaque ID into AudioEngine's voice table. 20-bit slot index +
// 12-bit generation guards against use-after-recycle (a stopped voice slot
// reused by a later PlayClip would otherwise alias the old handle).
struct VoiceHandle
{
    uint32_t value = 0;   // 0 = invalid

    static constexpr uint32_t kIndexBits      = 20;
    static constexpr uint32_t kGenerationBits = 12;
    static constexpr uint32_t kIndexMask      = (1u << kIndexBits) - 1u;
    static constexpr uint32_t kGenerationMask = (1u << kGenerationBits) - 1u;

    static VoiceHandle Make(uint32_t index, uint32_t generation)
    {
        VoiceHandle h;
        h.value = (index & kIndexMask)
                | ((generation & kGenerationMask) << kIndexBits);
        return h;
    }

    uint32_t Index()      const { return value & kIndexMask; }
    uint32_t Generation() const { return (value >> kIndexBits) & kGenerationMask; }
    bool     IsValid()    const { return value != 0; }
};

// Parameters used when starting playback. Most fields mirror what
// AudioSourceComponent stores; events that play one-shots without a source
// component (UI clicks) build this struct directly.
struct PlayParams
{
    BusType          bus           = BusType::SFX;
    float            volume        = 1.0f;
    float            pitch         = 1.0f;     // 1.0 = original pitch
    bool             loop          = false;
    bool             is3D          = false;

    // 3D attenuation envelope (only consulted when is3D == true).
    DirectX::XMFLOAT3 position     = { 0.f, 0.f, 0.f };
    float            minDistance   = 1.0f;
    float            maxDistance   = 50.0f;
    AttenuationCurve curve         = AttenuationCurve::Linear;
};

// Parameters that can be tweaked on a live voice (bus is fixed at PlayClip).
struct VoiceParams
{
    float volume = 1.0f;
    float pitch  = 1.0f;
};

} // namespace Audio
