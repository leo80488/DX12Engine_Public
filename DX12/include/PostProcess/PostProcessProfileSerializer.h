#pragma once

// PostProcessProfileSerializer — save/load a PostProcessProfile to/from the
// .ppprofile asset format (the shared post-process look referenced by volumes
// and the engine default).
//
// .ppprofile blob layout (matches the engine's other text assets):
//   [AssetHeader (24 B)] [PostProcessProfileMetadata (16 B)] [text payload (null-terminated)]
//
// Text payload: one "group_member=value" line per OVERRIDDEN property. The mere
// presence of a key encodes overrideState=true; absent keys keep their default
// and stay non-overriding. The key/value set is driven entirely by the X-macro
// in PostProcessProperties.inl, so new properties serialize automatically.

#include <string>

namespace PostProcess
{
struct PostProcessProfile;

// Write profile to a .ppprofile file at path. Returns true on success.
bool SaveProfile(const PostProcessProfile& profile, const std::string& path);

// Read a .ppprofile file at path into profile (read via AssetFS so packed
// builds resolve from the .ipak). Missing keys keep profile's current values.
// Returns false only on I/O / header-validation failure.
bool LoadProfile(const std::string& path, PostProcessProfile& profile);

} // namespace PostProcess
