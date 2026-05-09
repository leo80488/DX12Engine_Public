#include "Audio/AudioEngine.h"
#include "System/Log.h"

#pragma comment(lib, "xaudio2.lib")

namespace Audio
{

namespace
{

// Helper: log + return false if HRESULT failed.
bool Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        LOG_ERROR("[Audio] %s failed (hr=0x%08lX)", what, static_cast<long>(hr));
        return false;
    }
    return true;
}

} // anonymous

AudioEngine::~AudioEngine() { Shutdown(); }

bool AudioEngine::Initialize()
{
    if (m_initialized) return true;

    // COM is initialised by the Window already (CoInitializeEx with
    // COINIT_MULTITHREADED in main). XAudio2Create works regardless.
    HRESULT hr = XAudio2Create(m_xaudio2.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (!Check(hr, "XAudio2Create")) return false;

    hr = m_xaudio2->CreateMasteringVoice(&m_master);
    if (!Check(hr, "CreateMasteringVoice")) { Shutdown(); return false; }

    // Query master voice details for X3DAudio.
    XAUDIO2_VOICE_DETAILS masterDetails{};
    m_master->GetVoiceDetails(&masterDetails);
    m_masterChannels = masterDetails.InputChannels;

    DWORD channelMask = 0;
    if (FAILED(m_master->GetChannelMask(&channelMask)))
    {
        // Fall back to stereo if the driver doesn't report a mask.
        channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }
    m_channelMask = channelMask;

    // ---- Submix bus graph ---------------------------------------------------
    // Master is the mastering voice itself; the other 5 submixes feed it.
    // SendList = { master } sets each submix's output to master implicitly
    // (XAudio2 routes to the mastering voice when no SendList is provided).
    m_submixes[(size_t)BusType::Master] = nullptr;  // sentinel — not used
    auto MakeSubmix = [&](BusType bus, const char* name) -> bool
    {
        IXAudio2SubmixVoice* v = nullptr;
        hr = m_xaudio2->CreateSubmixVoice(
            &v, masterDetails.InputChannels, masterDetails.InputSampleRate,
            0, 0, nullptr, nullptr);
        if (!Check(hr, name)) return false;
        m_submixes[(size_t)bus] = v;
        return true;
    };
    if (!MakeSubmix(BusType::Music,   "CreateSubmixVoice(Music)"))   { Shutdown(); return false; }
    if (!MakeSubmix(BusType::SFX,     "CreateSubmixVoice(SFX)"))     { Shutdown(); return false; }
    if (!MakeSubmix(BusType::Voice,   "CreateSubmixVoice(Voice)"))   { Shutdown(); return false; }
    if (!MakeSubmix(BusType::Ambient, "CreateSubmixVoice(Ambient)")) { Shutdown(); return false; }
    if (!MakeSubmix(BusType::UI,      "CreateSubmixVoice(UI)"))      { Shutdown(); return false; }

    // ---- X3DAudio init ------------------------------------------------------
    hr = X3DAudioInitialize(m_channelMask, X3DAUDIO_SPEED_OF_SOUND, m_x3dInstance);
    if (!Check(hr, "X3DAudioInitialize")) { Shutdown(); return false; }

    // Default listener — origin, looking down +Z, head up +Y. World units = metres.
    m_listener            = {};
    m_listener.OrientFront = { 0.f, 0.f, 1.f };
    m_listener.OrientTop   = { 0.f, 1.f, 0.f };

    // Voice slots are a fixed-size array (see AudioEngine.h). Slot 0 is the
    // invalid-handle sentinel — push the rest onto the free list.
    m_freeSlots.clear();
    m_freeSlots.reserve(kMaxVoices);
    for (uint32_t i = kMaxVoices; i-- > 1;) m_freeSlots.push_back(i);

    m_initialized = true;
    LOG_INFO("[Audio] AudioEngine initialized (master ch=%u rate=%u)",
             m_masterChannels, masterDetails.InputSampleRate);
    return true;
}

void AudioEngine::Shutdown()
{
    if (!m_xaudio2 && !m_master) return;

    // Tear down all live source voices first; their callback objects must
    // outlive the voice (XAudio2 may still be flushing on its thread).
    {
        std::lock_guard<std::mutex> lk(m_voicesMutex);
        for (auto& rec : m_voices)
        {
            if (rec.source)
            {
                rec.source->Stop(0);
                rec.source->FlushSourceBuffers();
                rec.source->DestroyVoice();
                rec.source = nullptr;
            }
            rec.callback.reset();
            rec.clip = nullptr;
            rec.inUse = false;
        }
        m_freeSlots.clear();
    }

    // Submix voices.
    for (auto*& v : m_submixes)
    {
        if (v) { v->DestroyVoice(); v = nullptr; }
    }

    if (m_master) { m_master->DestroyVoice(); m_master = nullptr; }
    m_xaudio2.Reset();
    m_initialized = false;
    LOG_INFO("[Audio] AudioEngine shut down");
}

void AudioEngine::Update(float /*dt*/)
{
    if (!m_initialized) return;

    // Reclaim voices flagged finished by the audio worker thread.
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    for (uint32_t i = 1; i < m_voices.size(); ++i)
    {
        VoiceRecord& rec = m_voices[i];
        if (!rec.inUse) continue;
        if (!rec.finished.load(std::memory_order_acquire)) continue;

        if (rec.source)
        {
            rec.source->Stop(0);
            rec.source->FlushSourceBuffers();
            rec.source->DestroyVoice();
            rec.source = nullptr;
        }
        rec.callback.reset();
        rec.clip = nullptr;
        rec.finished.store(false, std::memory_order_relaxed);
        ReleaseSlot(i);
    }
}

uint32_t AudioEngine::AcquireSlot()
{
    if (m_freeSlots.empty()) return 0;
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    VoiceRecord& rec = m_voices[slot];
    rec.inUse = true;
    // Generation bumps every reuse so stale handles fail ResolveHandle.
    if (++rec.generation == 0) rec.generation = 1;
    return slot;
}

void AudioEngine::ReleaseSlot(uint32_t slot)
{
    if (slot == 0 || slot >= m_voices.size()) return;
    VoiceRecord& rec = m_voices[slot];
    rec.inUse = false;
    m_freeSlots.push_back(slot);
}

bool AudioEngine::ResolveHandle(VoiceHandle h, VoiceRecord*& out)
{
    out = nullptr;
    if (!h.IsValid()) return false;
    const uint32_t idx = h.Index();
    if (idx == 0 || idx >= m_voices.size()) return false;
    VoiceRecord& rec = m_voices[idx];
    if (!rec.inUse) return false;
    if ((rec.generation & VoiceHandle::kGenerationMask) != h.Generation()) return false;
    out = &rec;
    return true;
}

bool AudioEngine::ResolveHandle(VoiceHandle h, const VoiceRecord*& out) const
{
    out = nullptr;
    if (!h.IsValid()) return false;
    const uint32_t idx = h.Index();
    if (idx == 0 || idx >= m_voices.size()) return false;
    const VoiceRecord& rec = m_voices[idx];
    if (!rec.inUse) return false;
    if ((rec.generation & VoiceHandle::kGenerationMask) != h.Generation()) return false;
    out = &rec;
    return true;
}

void AudioEngine::RouteToBus(IXAudio2SourceVoice* voice, BusType bus)
{
    // Master is the implicit destination. For other buses we wire the source
    // → submix → master. CreateSourceVoice already routes to master by default;
    // we override the SendList only when targeting a non-master bus.
    if (bus == BusType::Master) return;

    IXAudio2SubmixVoice* submix = m_submixes[(size_t)bus];
    if (!submix) return;

    XAUDIO2_SEND_DESCRIPTOR sendDesc{};
    sendDesc.Flags     = 0;
    sendDesc.pOutputVoice = submix;
    XAUDIO2_VOICE_SENDS sends{};
    sends.SendCount = 1;
    sends.pSends    = &sendDesc;
    voice->SetOutputVoices(&sends);
}

VoiceHandle AudioEngine::PlayClip(const AudioClipResource* clip, const PlayParams& params)
{
    if (!m_initialized || !clip) return {};

    // v1 acceptance gate — matches AudioClipLoader. Compressed/streaming
    // clips will surface here once decoders / streamer-thread paths land;
    // until then drop them rather than feeding XAudio2 garbage data.
    if (!clip->IsPCM() || !clip->IsFullyLoaded() || clip->data.empty())
    {
        LOG_WARNING("[Audio] PlayClip rejected unsupported clip "
                    "(codec=%u strategy=%u dataBytes=%zu)",
                    clip->header.codec, clip->header.strategy, clip->data.size());
        return {};
    }

    // Acquire a slot before calling CreateSourceVoice — keeps the table
    // consistent if creation fails.
    uint32_t slot = 0;
    {
        std::lock_guard<std::mutex> lk(m_voicesMutex);
        slot = AcquireSlot();
    }
    if (slot == 0)
    {
        LOG_WARNING("[Audio] Out of voice slots; dropping play");
        return {};
    }

    VoiceRecord& rec = m_voices[slot];
    rec.clip        = clip;
    rec.srcChannels = clip->format.nChannels;
    rec.bus         = params.bus;
    rec.is3D        = params.is3D;
    rec.minDistance = params.minDistance;
    rec.maxDistance = params.maxDistance;
    rec.curve       = params.curve;
    rec.finished.store(false, std::memory_order_relaxed);

    rec.callback = std::make_unique<VoiceCallback>();
    rec.callback->finishedFlag = &rec.finished;
    rec.callback->loop         = params.loop;

    IXAudio2SourceVoice* src = nullptr;
    HRESULT hr = m_xaudio2->CreateSourceVoice(
        &src, &clip->format,
        0, XAUDIO2_DEFAULT_FREQ_RATIO, rec.callback.get(), nullptr, nullptr);
    if (!Check(hr, "CreateSourceVoice"))
    {
        rec.callback.reset();
        rec.clip = nullptr;
        std::lock_guard<std::mutex> lk(m_voicesMutex);
        ReleaseSlot(slot);
        return {};
    }

    rec.source = src;
    RouteToBus(src, params.bus);

    src->SetVolume(params.volume);
    src->SetFrequencyRatio(params.pitch);

    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = static_cast<UINT32>(clip->data.size());
    buf.pAudioData = clip->data.data();
    buf.Flags      = XAUDIO2_END_OF_STREAM;
    buf.LoopCount  = params.loop ? XAUDIO2_LOOP_INFINITE : 0;
    src->SubmitSourceBuffer(&buf);
    src->Start(0);

    return VoiceHandle::Make(slot, rec.generation);
}

void AudioEngine::StopVoice(VoiceHandle h, float fadeOutSec)
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (!ResolveHandle(h, rec) || !rec->source) return;

    if (fadeOutSec > 0.0f)
    {
        // No real fade ramp yet — XAudio2 has no direct linear-volume-fade
        // call; a real fade would post a SetVolume curve through an effect
        // chain. v1: just stop. The fadeOutSec parameter stays in the API so
        // callers don't change later.
        (void)fadeOutSec;
    }
    rec->source->Stop(0);
    // Mark finished so the next Update() will reclaim the slot. We don't
    // teardown here because the audio worker thread may still be touching
    // the buffer; deferring keeps the lifetime model uniform.
    rec->finished.store(true, std::memory_order_release);
}

void AudioEngine::SetVoiceVolume(VoiceHandle h, float volume)
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (ResolveHandle(h, rec) && rec->source) rec->source->SetVolume(volume);
}

void AudioEngine::SetVoicePitch(VoiceHandle h, float pitch)
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (ResolveHandle(h, rec) && rec->source) rec->source->SetFrequencyRatio(pitch);
}

bool AudioEngine::IsVoicePlaying(VoiceHandle h) const
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    const VoiceRecord* rec = nullptr;
    if (!ResolveHandle(h, rec)) return false;
    return rec->source != nullptr && !rec->finished.load(std::memory_order_acquire);
}

void AudioEngine::SetListener(const X3DAUDIO_LISTENER& listener)
{
    // No mutex — listener is read only from Apply3D (also main thread). If a
    // worker thread ever calls Apply3D this needs protecting.
    m_listener = listener;
}

void AudioEngine::Apply3D(VoiceHandle h, const X3DAUDIO_EMITTER& emitter)
{
    if (!m_initialized) return;

    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (!ResolveHandle(h, rec) || !rec->source || !rec->is3D) return;

    // Output channel count depends on the bus. Master has m_masterChannels;
    // any submix we created also uses m_masterChannels (we built them with
    // masterDetails.InputChannels). So this is uniform.
    const UINT32 srcChannels = rec->srcChannels ? rec->srcChannels : 1;
    const UINT32 dstChannels = m_masterChannels;

    float matrix[8 * 8] = {};   // up to 8x8 — covers stereo, 5.1, 7.1
    if (srcChannels * dstChannels > 8 * 8) return;

    X3DAUDIO_DSP_SETTINGS dsp{};
    dsp.SrcChannelCount     = srcChannels;
    dsp.DstChannelCount     = dstChannels;
    dsp.pMatrixCoefficients = matrix;

    // Local copy of emitter so we can stamp curve / distance fields in case
    // the caller didn't fill them.
    X3DAUDIO_EMITTER e = emitter;
    if (e.ChannelCount == 0) e.ChannelCount = srcChannels;
    if (e.CurveDistanceScaler <= 0.f)
        e.CurveDistanceScaler = rec->maxDistance > 0.f ? rec->maxDistance : 1.f;
    if (e.DopplerScaler <= 0.f) e.DopplerScaler = 1.f;

    X3DAudioCalculate(
        m_x3dInstance, &m_listener, &e,
        X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_DOPPLER,
        &dsp);

    rec->source->SetOutputMatrix(nullptr, srcChannels, dstChannels, matrix);
    rec->source->SetFrequencyRatio(dsp.DopplerFactor);
}

void AudioEngine::SetBusVolume(BusType bus, float volume)
{
    if (!m_initialized) return;
    if (bus == BusType::Master)
    {
        if (m_master) m_master->SetVolume(volume);
        return;
    }
    IXAudio2SubmixVoice* sub = m_submixes[(size_t)bus];
    if (sub) sub->SetVolume(volume);
}

} // namespace Audio
