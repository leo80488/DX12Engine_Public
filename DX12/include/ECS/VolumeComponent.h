#pragma once

// ECS::VolumeComponent — attach to an entity to make it drive a post-process
// volume. The entity's GlobalTransform provides the world-space center each
// frame (so a Volume follows its parent, animates with its mesh, etc.).
//
// The Volume::center field held inside the component is *ignored* at gather
// time — EntityVolumeSource overrides it with the transform's translation
// before computing weight. Edit `extents`, `blendDistance`, `priority`,
// `override`, etc. via the Editor inspector.
//
// Phase 5 note: no scene serialization yet; the component lives only in
// runtime memory. Persistence will be a follow-up.

#include "PostProcess/Volume.h"

namespace ECS
{

struct VolumeComponent
{
    PostProcess::Volume volume;
};

} // namespace ECS
