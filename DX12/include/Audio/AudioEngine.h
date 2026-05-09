#pragma once

// AudioEngine — XAudio2 wrapper. The lowest layer of the audio stack: knows
// nothing about ECS, events, or scenes. Spec: audio_system_architecture.md §2.
//
// Owns:
//   - IXAudio2 + mastering voice
//   - One submix voice per BusType (Music / SFX / Voice / Ambient / UI),
//     all sending to master. BusType::Master is the mastering voice.
//   - A live-voice table (slot index + generation packed into VoiceHandle).
//     v1 creates IXAudio2SourceVoice on demand and destroys on completion;
//     a proper voice pool (AVoid CreateSourceVoice churn) lives behind a
//     TODO and arrives once we have measurable cost.
//   - X3DAudio instance + cached listener pose.
//
// Clip storage is OUT of scope for this class — AudioClipResource lives
// inside the engine's resource manager and is reached via AudioClipSystem
// (path-deduplicated handle registry). PlayClip just borrows the resource
// pointer for the lifetime of the voice.

#include "Audio/AudioTypes.h"
#include "Audio/AudioClipResource.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <x3daudio.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Audio
{

class AudioEngine
{
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Initialize();
    void Shutdown();

    // Per-frame sync point. Reclaims source voices flagged as finished by the
    // XAudio2 worker thread (callbacks set an atomic; this drains them).
    void Update(float dt);

    // Start playback against a resolved clip resource. The caller (typically
    // AudioSystem after AudioClipSystem reports ready) owns the resource's
    // lifetime — the engine only borrows the format pointer and PCM bytes
    // for the duration of the voice. Returns an invalid VoiceHandle on
    // failure (null clip, unsupported codec, source-voice creation failed,
    // voice slot exhausted).
    VoiceHandle PlayClip(const AudioClipResource* clip, const PlayParams& params);

    // Live-voice control — all are no-ops if the handle is stale.
    void StopVoice(VoiceHandle h, float fadeOutSec = 0.0f);
    void SetVoiceVolume(VoiceHandle h, float volume);
    void SetVoicePitch (VoiceHandle h, float pitch);

    // Returns false if the slot was reused / released.
    bool IsVoicePlaying(VoiceHandle h) const;

    // ---- 3D ----------------------------------------------------------------
    // Listener pose is a single global (last writer wins). Audio3DSystem
    // refreshes this every tick from the active AudioListenerComponent.
    void SetListener(const X3DAUDIO_LISTENER& listener);

    // Apply a 3D emitter calculation to a live voice. Computes matrix /
    // doppler / LPF coefficients via X3DAudio and feeds them into the voice's
    // output matrix and frequency ratio.
    void Apply3D(VoiceHandle h, const X3DAUDIO_EMITTER& emitter);

    // ---- Mixer -------------------------------------------------------------
    // Volume is a linear gain (0..N). 1.0 = unity.
    void SetBusVolume(BusType bus, float volume);

private:
    // ------------------------------------------------------------------------
    // Per-voice record. Lives in m_voices indexed by VoiceHandle::Index().
    // generation increments when the slot is recycled — handles with a
    // mismatched generation are silently dropped (use-after-free safety).
    // ------------------------------------------------------------------------
    struct VoiceCallback;

    struct VoiceRecord
    {
        IXAudio2SourceVoice*             source       = nullptr;
        // Borrowed — owned by ResourceManager via AudioClipSystem. The
        // resource is ref-counted at the system level so PCM stays valid
        // while a voice references it. Never holds ownership here.
        const AudioClipResource*         clip         = nullptr;
        // Source-channel count cached at PlayClip time so Apply3D can resolve
        // matrix dims without dereferencing `clip`.
        uint16_t                         srcChannels  = 1;
        std::unique_ptr<VoiceCallback>   callback;
        BusType                          bus          = BusType::SFX;
        bool                             is3D         = false;
        float                            minDistance  = 1.0f;
        float                            maxDistance  = 50.0f;
        AttenuationCurve                 curve        = AttenuationCurve::Linear;
        uint32_t                         generation   = 1;     // 0 = handle invalid
        // Set by VoiceCallback::OnStreamEnd on the XAudio2 worker thread; the
        // main thread's Update polls this and tears the voice down.
        std::atomic<bool>                finished     { false };
        bool                             inUse        = false;
    };

    // XAudio2 callback — fires on the audio worker thread. Keep work minimal:
    // only flip an atomic. The main thread reclaims the voice next Update.
    struct VoiceCallback : public IXAudio2VoiceCallback
    {
        std::atomic<bool>* finishedFlag = nullptr;
        bool               loop         = false;

        void STDMETHODCALLTYPE OnStreamEnd() noexcept override
        {
            if (!loop && finishedFlag) finishedFlag->store(true, std::memory_order_release);
        }
        // Other callbacks unused but must be defined for the vtable.
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd()         noexcept override {}
        void STDMETHODCALLTYPE OnBufferStart (void*)              noexcept override {}
        void STDMETHODCALLTYPE OnBufferEnd   (void*)              noexcept override {}
        void STDMETHODCALLTYPE OnLoopEnd     (void*)              noexcept override {}
        void STDMETHODCALLTYPE OnVoiceError  (void*, HRESULT)     noexcept override {}
    };

    // Internal helpers — must be called with m_voicesMutex held by the caller
    // (or before/after the voice has been published to the audio thread).
    uint32_t AcquireSlot();
    void     ReleaseSlot(uint32_t slot);
    bool     ResolveHandle(VoiceHandle h, VoiceRecord*& out);
    bool     ResolveHandle(VoiceHandle h, const VoiceRecord*& out) const;

    // Send-list helper: routes a fresh source voice into the bus submix.
    void RouteToBus(IXAudio2SourceVoice* voice, BusType bus);

    // ----------------------- core XAudio2 objects ---------------------------
    Microsoft::WRL::ComPtr<IXAudio2>           m_xaudio2;
    IXAudio2MasteringVoice*                    m_master = nullptr;
    std::array<IXAudio2SubmixVoice*, (size_t)BusType::Count> m_submixes{};

    // ----------------------- voice table ------------------------------------
    // Mutex guards m_voices + m_freeSlots. Acquired briefly during PlayClip,
    // Update, Apply3D, etc. — never held across XAudio2 API calls that block.
    //
    // Fixed-size storage: VoiceRecord holds non-movable members (std::atomic,
    // std::unique_ptr) so std::vector growth paths refuse to compile. The
    // slot count is bounded at engine init anyway, so a std::array is the
    // natural fit.
    static constexpr uint32_t                  kMaxVoices = 128;
    mutable std::mutex                         m_voicesMutex;
    std::array<VoiceRecord, kMaxVoices>        m_voices;
    std::vector<uint32_t>                      m_freeSlots;

    // ----------------------- 3D ---------------------------------------------
    X3DAUDIO_HANDLE                            m_x3dInstance{};
    X3DAUDIO_LISTENER                          m_listener{};
    DWORD                                      m_channelMask = 0;
    UINT32                                     m_masterChannels = 2;

    bool                                       m_initialized = false;
};

} // namespace Audio
