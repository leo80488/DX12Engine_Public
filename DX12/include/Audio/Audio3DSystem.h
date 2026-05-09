#pragma once

// Audio3DSystem — feeds X3DAudio with this frame's listener pose + each live
// 3D voice's emitter pose, then asks AudioEngine to apply the resulting DSP
// (output matrix + doppler) to the source voice.
//
// Order: must run AFTER TransformSystem::Propagate so GlobalTransform is up
// to date for both the listener (camera) and emitters.
//
// Spec: audio_system_architecture.md §3.3.

#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <unordered_map>

namespace Audio
{

class AudioEngine;

class Audio3DSystem
{
public:
    void BindEngine(AudioEngine& engine) { m_engine = &engine; }
    void Update(World& world, float dt);

private:
    AudioEngine* m_engine = nullptr;

    // Cached previous emitter positions, used to derive velocity → doppler.
    // Map keyed on Entity is intentional: AudioSourceComponent dense order
    // can change as components add/remove, so we can't index by pool slot.
    struct EmitterCache
    {
        DirectX::XMFLOAT3 position { 0.f, 0.f, 0.f };
        bool              valid    = false;
    };
    std::unordered_map<Entity, EmitterCache> m_emitterCache;

    DirectX::XMFLOAT3 m_listenerPrevPos { 0.f, 0.f, 0.f };
    bool              m_listenerCached  = false;
};

} // namespace Audio
