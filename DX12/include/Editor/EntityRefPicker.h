#pragma once

// Inspector helpers for the GUID-based entity reference system.
//
// AttachmentRef is the cross-entity reference type defined in
// include/ECS/GuidComponent.h. Components that hold one (FollowCamera,
// AimCamera, CameraController, PlayerComponent today) get a target picker
// in their Inspector row via DrawAttachmentRef.
//
// GuidComponent gets a small read-only display + regenerate popup via
// DrawGuidComponentInspector. The "Stamp GUID" entry point is implicit:
// adding GuidComponent via the normal Add Component menu triggers the
// inspector, and BindAndStamp from any picker auto-adds the component
// on the target.

#include "ECS/ECS.h"

struct AttachmentRef;
struct GuidComponent;
class World;

namespace Editor
{

// Returns true if the user touched the ref this frame (binding, picking a
// new target, or clearing). selfEntity is excluded from the picker so a
// component can't bind to its own host entity.
bool DrawAttachmentRef(const char* label,
                       World& world,
                       Entity selfEntity,
                       AttachmentRef& ref);

void DrawGuidComponentInspector(GuidComponent& gc, World& world, Entity self);

} // namespace Editor
