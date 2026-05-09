#pragma once

// VmdImporter — standalone VMD (Vocaloid Motion Data) animation importer.
//
// Parses the binary VMD format produced by MikuMikuDance and converts bone
// keyframe tracks to the internal .ianim format.  Morph/face, camera, light,
// and shadow tracks are silently skipped; only bone tracks are extracted.
//
// Coordinate convention: identity (no axis negation), matching PmxImporter.
//   Position:   (x, y, z)_MMD → (x, y, z)  unchanged
//   Quaternion: (x, y, z, w)_MMD → (x, y, z, w)  unchanged
//
// Bone names are stored as Shift-JIS in VMD; they are converted to UTF-8
// (via Win32 MultiByteToWideChar) before writing into .ianim channels so they
// can be matched against skeleton bone names imported from PMX/FBX via Assimp.
//
// Supported variants:
//   VMD2 — "Vocaloid Motion Data 0002"  (20-byte model name)
//   VMD1 — "Vocaloid Motion Data file"  (10-byte model name)
//
// Registered with ResourceManager for: .vmd
// Produces: .ianim

#include "Resource/IImporter.h"

namespace Resource
{
    class VmdImporter : public IImporter
    {
    public:
        // Converts raw VMD bytes to a single-clip .ianim blob.
        // Returns an empty vector if the file is not a valid VMD or has no bone keyframes.
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override
        {
            return { ".vmd" };
        }

        const char* GetInternalExtension() const override { return ".ianim"; }
    };
}
