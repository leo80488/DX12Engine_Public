#pragma once

// AnimationSerializer — writes a full AnimationResource back out to a .ianim
// binary blob, including the tail AnimNotify section.
//
// This is the inverse of AnimationLoader::Load and produces a blob that is
// byte-compatible with what AnimationImporter emits for the bone/morph
// portion, PLUS the notify section (gated by ANIM_FLAG_HAS_NOTIFIES) so the
// animation editor can round-trip a clip's notify tracks back to disk.
//
// Used by:
//   * EditorLayer animation-timeline "Save" — re-serialize the loaded
//     AnimationResource (bones + morphs are untouched; notifyTracks edited).
//
// The bone/morph layout intentionally matches AnimationImporter.cpp /
// AnimationLoader.cpp exactly (per-clip: counts, duration/rate, name[64],
// channels, eventCount + events; per-morph: counts, duration/rate, name[64],
// channels).

#include <cstdint>
#include <vector>

namespace Resource
{
    class AnimationResource;

    // Serialize a full AnimationResource to a .ianim blob (header + metadata +
    // bone clips + morph clips + notify section). The offset flags are derived
    // from the per-clip positionsAreOffsets / rotationsAreOffsets so a VMD-
    // sourced clip round-trips its additive-pose semantics. Returns an empty
    // vector only if `res` has no clips and no morph clips.
    std::vector<uint8_t> SerializeAnimation(const AnimationResource& res);
}
