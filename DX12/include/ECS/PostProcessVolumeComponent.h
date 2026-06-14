#pragma once

// ECS::PostProcessVolumeComponent — attach to an entity (with a Transform) to
// make it a spatial post-process volume. The entity's GlobalTransform supplies
// the bounds: Box uses position + rotation + scale (an OBB, half-extents =
// scale); Sphere uses position + scale.x as radius. No bounds are stored on the
// component — the Transform is the single source of truth (design §4.1).
//
// The look is NOT embedded here: the volume references a shared PostProcessProfile
// resource by handle. profilePath is the serialization source of truth; on scene
// load it is resolved to `profile` via ProfileSystem::Acquire. A freshly created
// editor volume gets a runtime profile (ProfileSystem::CreateRuntime) until the
// artist saves it as a .ppprofile asset.

#include "PostProcess/PostProcessProfile.h"   // ProfileHandle

#include <cstdint>
#include <string>

namespace ECS
{

enum class PPVolumeShape : uint8_t
{
    Box,      // OBB from Transform position + rotation + scale (half-extents)
    Sphere,   // center + scale.x as radius
};

struct PostProcessVolumeComponent
{
    bool          isGlobal      = false;        // true = unbounded (ignores shape/bounds)
    PPVolumeShape shape         = PPVolumeShape::Box;
    float         priority      = 0.0f;         // higher blends later → wins
    float         blendWeight   = 1.0f;         // 0..1 master weight
    float         blendDistance = 1.0f;         // world units; falloff shell outside the bounds
    uint32_t      layerMask     = 0xFFFFFFFFu;  // AND-ed with the view's layer mask

    PostProcess::ProfileHandle profile;         // → shared PostProcessProfile resource
    std::string                profilePath;     // .ppprofile asset path (serialized)
};

} // namespace ECS
