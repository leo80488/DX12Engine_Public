#pragma once

// SceneLoadContext — per-load helper used by component deserializers to
// remap *legacy* raw Entity-id references stored in old scene files.
//
// New scene files serialize entity references as GUIDs (resolved through
// GuidRegistry). For back-compat with older `.iscn` files that still hold
// raw decimal entity ids (e.g. "camEntity=42"), SceneSerializer installs an
// `oldIdx → newEntity` map for the duration of one load. Migrated
// component deserializers (PlayerComponent.cameraEntity,
// CameraControllerComponent.followTarget, …) call ResolveLegacyEntityIdx
// during deserialize; once resolved they immediately stamp a GUID on the
// target via AttachmentRef::BindAndStamp, so the next save round-trips
// cleanly as GUID-only.
//
// thread_local so concurrent loads (future) don't trip over each other.

#include "ECS/ECS.h"

#include <cstdint>
#include <unordered_map>

namespace Resource
{

// Bind / unbind the legacy entity-idx map. Caller owns the map; passing
// nullptr clears the binding.
void SetLegacyEntityMap(const std::unordered_map<int, Entity>* map);

// Look up a legacy saved-idx → current Entity. Returns NullEntity when no
// map is bound or the idx isn't in it.
Entity ResolveLegacyEntityIdx(uint32_t oldIdx);

} // namespace Resource
