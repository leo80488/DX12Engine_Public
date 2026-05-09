#pragma once

// AudioClipResource — runtime form of a .aclip asset (`MAGIC_AUDIO`).
//
// Mirrors the texture / mesh / animation pattern: the resource owns the raw
// playable bytes plus enough metadata for the audio engine to start a voice
// and the systems above to react to cue points / loop ranges.
//
// v1 only ships the AUDIO_LOAD_FULL / AUDIO_CODEC_PCM combination — the data
// vector is the same PCM that XAudio2 will SubmitSourceBuffer. Compressed and
// streaming strategies are reserved (see AssetHeader.h enums) and arrive in
// a follow-up; the loader rejects any other codec/strategy explicitly.

#include "Resource/Resource.h"
#include "Resource/AssetHeader.h"   // AudioClipMetadata + enums

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmreg.h>                  // WAVEFORMATEX

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Audio
{

// CPU-side cue (a marker on a sample offset). Engine reads this when game
// code asks for "play from cue X" or animation notify wants to fire-and-
// forget exactly at a beat.
struct AudioCue
{
    uint32_t sampleOffset = 0;
    char     name[32]     = {};
};

// `class T : public Resource::Resource` makes the *unqualified* name
// `Resource` resolve to the base class inside this scope, which then breaks
// any later `Resource::FOO` enum reference in member functions. Use the
// global-qualified `::Resource::` everywhere below to dodge that lookup.
class AudioClipResource : public ::Resource::Resource
{
public:
    // Header copied out of AudioClipMetadata for fast access without re-
    // parsing the blob.
    ::Resource::AudioClipMetadata header{};

    // WAVEFORMATEX synthesised from the metadata so AudioEngine can hand it
    // straight to CreateSourceVoice (PCM only in v1).
    WAVEFORMATEX                format{};

    // Contiguous data block (raw PCM for FULL/PCM, compressed bytes for the
    // reserved Compressed / Streaming strategies). XAudio2 buffers reference
    // this directly; never resize after Load to keep the pointer stable.
    std::vector<uint8_t>        data;

    // Cue / marker list parsed from the source file.
    std::vector<AudioCue>       cues;

    // ---- Convenience accessors used by AudioEngine / AudioSystem ----------
    bool IsPCM() const { return header.codec == ::Resource::AUDIO_CODEC_PCM; }
    bool IsFullyLoaded() const { return header.strategy == ::Resource::AUDIO_LOAD_FULL; }

    std::span<const uint8_t> PCM() const { return { data.data(), data.size() }; }

    const AudioCue* FindCue(std::string_view name) const
    {
        for (const AudioCue& c : cues)
            if (name == c.name) return &c;
        return nullptr;
    }
};

} // namespace Audio
