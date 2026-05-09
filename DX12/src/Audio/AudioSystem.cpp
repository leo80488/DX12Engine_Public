#include "Audio/AudioSystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioClipSystem.h"
#include "Audio/AudioClipResource.h"
#include "Audio/AudioComponents.h"
#include "Audio/AudioEvents.h"
#include "System/EventBus.h"
#include "System/Log.h"

namespace Audio
{

AudioSystem::AudioSystem()  = default;
AudioSystem::~AudioSystem() { Unbind(); }

void AudioSystem::BindClipSystem(AudioClipSystem& clipSystem)
{
    m_clipSystem = &clipSystem;
}

void AudioSystem::BindEngine(AudioEngine& engine)
{
    m_engine = &engine;

    EventBus& bus = EventBus::Get();

    auto play = bus.Subscribe<PlaySoundEvent>(
        [this](const PlaySoundEvent& e) { OnPlaySound(e); });
    m_unsubscribers.emplace_back([play]{ EventBus::Get().Unsubscribe<PlaySoundEvent>(play); });

    auto stop = bus.Subscribe<StopSoundEvent>(
        [this](const StopSoundEvent& e) { OnStopSound(e); });
    m_unsubscribers.emplace_back([stop]{ EventBus::Get().Unsubscribe<StopSoundEvent>(stop); });

    auto sp = bus.Subscribe<SetAudioParamEvent>(
        [this](const SetAudioParamEvent& e) { OnSetParam(e); });
    m_unsubscribers.emplace_back([sp]{ EventBus::Get().Unsubscribe<SetAudioParamEvent>(sp); });

    auto bv = bus.Subscribe<BusVolumeChangedEvent>(
        [this](const BusVolumeChangedEvent& e) { OnBusVolume(e); });
    m_unsubscribers.emplace_back([bv]{ EventBus::Get().Unsubscribe<BusVolumeChangedEvent>(bv); });
}

void AudioSystem::BindWorld(World& world)
{
    m_world = &world;
    m_destroyListenerId = world.AddEntityDestroyListener(
        [this](Entity e) { OnEntityDestroyed(e); });
}

void AudioSystem::Unbind()
{
    for (auto& fn : m_unsubscribers) fn();
    m_unsubscribers.clear();

    if (m_world && m_destroyListenerId != 0)
    {
        m_world->RemoveEntityDestroyListener(m_destroyListenerId);
        m_destroyListenerId = 0;
    }
    m_world      = nullptr;
    m_engine     = nullptr;
    m_clipSystem = nullptr;
}

void AudioSystem::Update(World& world, float /*dt*/)
{
    if (!m_engine || !m_clipSystem) return;

    // Drain finished fire-and-forget refcounts. Done before the per-source
    // pass so a clip that finished this frame is freed for reuse.
    {
        size_t w = 0;
        for (size_t r = 0; r < m_pendingReleases.size(); ++r)
        {
            const PendingRelease& pr = m_pendingReleases[r];
            if (m_engine->IsVoicePlaying(pr.voice))
                m_pendingReleases[w++] = pr;
            else
                m_clipSystem->ReleaseClip(pr.handle);
        }
        m_pendingReleases.resize(w);
    }

    // Drive lifecycle from the AudioSourceComponent pool. Iterating the pool
    // directly (not GetEntities + GetComponent) is the fast path — see the
    // ECS pool-iteration guidance in the codebase.
    auto* pool = world.GetPool<AudioSourceComponent>();
    if (!pool) return;
    auto& ents = pool->Entities();
    auto& data = pool->Data();
    const size_t n = data.size();

    for (size_t i = 0; i < n; ++i)
    {
        const Entity e   = ents[i];
        AudioSourceComponent& src = data[i];

        // 1. Lazy clip acquisition. A component starts with clipHandle = invalid
        //    even if clipPath is set; the first Update after the path was
        //    populated calls AudioClipSystem to start an async load.
        if (!src.clipHandle.IsValid() && !src.clipPath.empty())
            src.clipHandle = m_clipSystem->AcquireClip(src.clipPath);

        // 2. Sync runtime state from the engine: a finished one-shot voice
        //    should flip the component's state back to Stopped.
        if (src.activeVoice.IsValid() && !m_engine->IsVoicePlaying(src.activeVoice))
        {
            src.activeVoice = {};
            src.state       = PlayState::Stopped;
        }

        // 3. Auto-start newly added components flagged playOnEnable. We hold
        //    the playOnEnable flag set until the resource is actually ready
        //    so a frame-1 component on a slow disk still plays once loaded;
        //    once we successfully start the voice, the flag is cleared.
        if (src.playOnEnable
            && src.state == PlayState::Stopped
            && !src.activeVoice.IsValid()
            && src.clipHandle.IsValid()
            && m_clipSystem->IsReady(src.clipHandle))
        {
            const AudioClipResource* clip = m_clipSystem->GetResource(src.clipHandle);
            if (clip)
            {
                PlayParams pp;
                pp.bus         = src.bus;
                pp.volume      = src.volume;
                pp.pitch       = src.pitch;
                pp.loop        = src.loop;
                pp.is3D        = src.is3D;
                pp.minDistance = src.minDistance;
                pp.maxDistance = src.maxDistance;
                pp.curve       = src.curve;
                const VoiceHandle h = m_engine->PlayClip(clip, pp);
                if (h.IsValid())
                {
                    src.activeVoice  = h;
                    src.state        = PlayState::Playing;
                    src.playOnEnable = false;
                }
            }
            else
            {
                // Resource ready but null — load failed. Clear the flag so we
                // don't pump the same retry every frame.
                src.playOnEnable = false;
            }
        }
        (void)e;
    }
}

void AudioSystem::OnPlaySound(const PlaySoundEvent& e)
{
    if (!m_engine || !m_clipSystem) return;

    // Resolve clip identity. Acquiring is cheap (path-deduped) and stamps a
    // refCount that ReleaseClip later balances; PlaySoundEvents that target
    // an entity attribute that ref to the component, so a destroyed entity
    // releases automatically. Fire-and-forget plays drop the ref immediately
    // after PlayClip returns.
    Resource::AudioHandle handle = m_clipSystem->AcquireClip(e.clipPath);
    if (!handle.IsValid()) return;

    if (!m_clipSystem->IsReady(handle))
    {
        // The clip is still loading. Drop fire-and-forget plays — we don't
        // have an entity to remember the request on. Entity-targeted events
        // attach the handle so playOnEnable can fire once loading completes.
        if (e.entity.entity == NullEntity || !m_world || !m_world->IsHandleValid(e.entity))
        {
            m_clipSystem->ReleaseClip(handle);
            return;
        }
        AudioSourceComponent* src = m_world->GetComponent<AudioSourceComponent>(e.entity.entity);
        if (!src) { m_clipSystem->ReleaseClip(handle); return; }
        if (src->clipHandle.IsValid() && src->clipHandle != handle)
            m_clipSystem->ReleaseClip(src->clipHandle);
        src->clipPath     = e.clipPath;
        src->clipHandle   = handle;
        src->bus          = e.bus;
        src->volume       = e.volume;
        src->pitch        = e.pitch;
        src->loop         = e.loop;
        src->is3D         = e.is3D;
        src->playOnEnable = true;     // Update() will fire once the clip is ready
        return;
    }

    const AudioClipResource* clip = m_clipSystem->GetResource(handle);
    if (!clip) { m_clipSystem->ReleaseClip(handle); return; }

    PlayParams pp;
    pp.bus         = e.bus;
    pp.volume      = e.volume;
    pp.pitch       = e.pitch;
    pp.loop        = e.loop;
    pp.is3D        = e.is3D;
    pp.position    = e.position;
    pp.minDistance = e.minDistance;
    pp.maxDistance = e.maxDistance;
    pp.curve       = e.curve;

    // Entity-targeted: mirror onto its AudioSourceComponent + own a refcount
    // off the component (released on destroy or when the path changes).
    if (e.entity.entity != NullEntity && m_world && m_world->IsHandleValid(e.entity))
    {
        AudioSourceComponent* src = m_world->GetComponent<AudioSourceComponent>(e.entity.entity);
        if (src)
        {
            if (src->activeVoice.IsValid()) m_engine->StopVoice(src->activeVoice, 0.f);
            if (src->clipHandle.IsValid() && src->clipHandle != handle)
                m_clipSystem->ReleaseClip(src->clipHandle);
            src->clipPath   = e.clipPath;
            src->clipHandle = handle;
        }
        else
        {
            // No component to attach the refcount to — release the clone.
            m_clipSystem->ReleaseClip(handle);
        }

        const VoiceHandle h = m_engine->PlayClip(clip, pp);
        if (src && h.IsValid())
        {
            src->activeVoice = h;
            src->state       = PlayState::Playing;
        }
        return;
    }

    // Fire-and-forget. The engine borrows `clip->data` for the lifetime of
    // the voice — releasing the handle synchronously would free that buffer
    // out from under XAudio2. Stash the pairing instead and let Update drop
    // the refcount once the voice reports finished.
    const VoiceHandle vh = m_engine->PlayClip(clip, pp);
    if (vh.IsValid())
        m_pendingReleases.push_back({ vh, handle });
    else
        m_clipSystem->ReleaseClip(handle);
}

void AudioSystem::OnStopSound(const StopSoundEvent& e)
{
    if (!m_engine) return;

    if (e.voice.IsValid())
    {
        m_engine->StopVoice(e.voice, e.fadeOutSec);
        return;
    }
    if (e.entity.entity != NullEntity && m_world && m_world->IsHandleValid(e.entity))
    {
        if (auto* src = m_world->GetComponent<AudioSourceComponent>(e.entity.entity))
        {
            if (src->activeVoice.IsValid())
                m_engine->StopVoice(src->activeVoice, e.fadeOutSec);
            src->state       = PlayState::Stopped;
            src->activeVoice = {};
        }
    }
}

void AudioSystem::OnSetParam(const SetAudioParamEvent& e)
{
    if (!m_engine) return;

    VoiceHandle target = e.voice;
    if (!target.IsValid() && e.entity.entity != NullEntity
        && m_world && m_world->IsHandleValid(e.entity))
    {
        if (auto* src = m_world->GetComponent<AudioSourceComponent>(e.entity.entity))
            target = src->activeVoice;
    }
    if (!target.IsValid()) return;

    switch (e.param)
    {
        case SetAudioParamEvent::Volume: m_engine->SetVoiceVolume(target, e.value); break;
        case SetAudioParamEvent::Pitch:  m_engine->SetVoicePitch (target, e.value); break;
    }
}

void AudioSystem::OnBusVolume(const BusVolumeChangedEvent& e)
{
    if (m_engine) m_engine->SetBusVolume(e.bus, e.volume);
}

void AudioSystem::OnEntityDestroyed(Entity e)
{
    // Stop any voice still bound to a destroyed entity's source AND release
    // its clip refcount — the component goes away in a moment, so this is
    // the last chance to balance the AcquireClip from PlaySound or Update.
    if (!m_engine || !m_world) return;
    if (auto* src = m_world->GetComponent<AudioSourceComponent>(e))
    {
        if (src->activeVoice.IsValid())
            m_engine->StopVoice(src->activeVoice, 0.f);
        if (m_clipSystem && src->clipHandle.IsValid())
            m_clipSystem->ReleaseClip(src->clipHandle);
        src->clipHandle  = Resource::kInvalidAudioHandle;
        src->activeVoice = {};
        src->state       = PlayState::Stopped;
    }
}

} // namespace Audio
