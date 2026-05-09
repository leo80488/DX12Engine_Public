#pragma once

// AudioSystem — turns ECS / event activity into AudioEngine calls.
//   • Subscribes to PlaySoundEvent / StopSoundEvent / SetAudioParamEvent /
//     BusVolumeChangedEvent on the global EventBus.
//   • Sweeps AudioSourceComponent each frame: handles `playOnEnable`,
//     reclaims voices that finished, syncs PlayState back into the component.
//
// Spec: audio_system_architecture.md §3.2.

#include "ECS/ECS.h"
#include "Audio/AudioTypes.h"
#include "Resource/SystemHandles.h"
#include <cstddef>
#include <functional>
#include <vector>

namespace Audio
{

class AudioEngine;
class AudioClipSystem;

class AudioSystem
{
public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Bind* must be called before Update / event handlers fire. The clip
    // system is the resolver path-string → AudioHandle → AudioClipResource;
    // without it events never play (we'd have no source bytes).
    void BindEngine    (AudioEngine&     engine);
    void BindClipSystem(AudioClipSystem& clipSystem);
    void BindWorld     (World&           world);
    void Unbind();

    // Per-frame tick. Iterates AudioSourceComponent pool only — no need to
    // walk every entity.
    void Update(World& world, float dt);

private:
    void OnPlaySound (const struct PlaySoundEvent&);
    void OnStopSound (const struct StopSoundEvent&);
    void OnSetParam  (const struct SetAudioParamEvent&);
    void OnBusVolume (const struct BusVolumeChangedEvent&);
    void OnEntityDestroyed(Entity e);

    AudioEngine*     m_engine     = nullptr;
    AudioClipSystem* m_clipSystem = nullptr;
    World*           m_world      = nullptr;

    // Fire-and-forget lifetime: PlaySoundEvent without an entity owner has
    // no component to anchor the AudioHandle's refcount, but releasing the
    // handle synchronously would free PCM that XAudio2 is still reading. We
    // stash (voice, handle) pairs here and release on the first Update that
    // sees IsVoicePlaying == false.
    struct PendingRelease
    {
        VoiceHandle           voice;
        Resource::AudioHandle handle;
    };
    std::vector<PendingRelease> m_pendingReleases;

    // Subscription handles — held so Unbind() can detach cleanly. Each entry
    // is a std::function<void()> closure that calls EventBus::Unsubscribe<E>.
    std::vector<std::function<void()>> m_unsubscribers;
    uint32_t                            m_destroyListenerId = 0;
};

} // namespace Audio
