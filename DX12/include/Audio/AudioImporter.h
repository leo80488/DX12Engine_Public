#pragma once

// AudioImporter — Editor-only converter from raw audio source files to the
// engine's internal .aclip blob. Pattern matches AnimationImporter etc.:
//   IImporter::Import(srcBytes) → vector<uint8_t> with [AssetHeader |
//   AudioClipMetadata | data + cue table].
//
// v1 source coverage:
//   .wav  — RIFF / WAVE_FORMAT_PCM only (8/16/24/32-bit). Parses any
//            cue/labl chunks present and writes them as AudioCueRecord
//            entries on the tail of the data block.
//
// .ogg / .flac / .mp3 reserved — tracked under audio_system_architecture.md
// §6.2 build-pipeline scope. Adding them is an Import() switch + a third-
// party decoder pulled in behind the editor build.

#include "Resource/IImporter.h"

namespace Audio
{
    class AudioImporter : public Resource::IImporter
    {
    public:
        std::vector<uint8_t> Import(const std::string&                sourcePath,
                                    const std::vector<uint8_t>&       sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override
        {
            return { ".wav" };
        }

        const char* GetInternalExtension() const override { return ".aclip"; }
    };
}
