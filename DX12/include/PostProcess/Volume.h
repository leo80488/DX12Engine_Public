#pragma once

// PostProcess::Volume — a spatial trigger that overrides selected stages of
// the authoritative ParameterStore when the camera is inside (or within
// blendDistance of) its shape.
//
// Phase 3 scope:
//   - Global / Box / Sphere shapes
//   - Per-stage whole-block override via std::optional
//   - Weight = 1 at/inside, linear falloff to 0 over blendDistance outside
//   - Priority orders blend application (low → high; higher priority layers on top)
//   - Blend is always weight-lerp (Override mode = Lerp at w=1)
//
// Volumes do not persist to disk in Phase 3. Editor-created volumes live only
// for the session.

#include "PostProcess/PostProcessParams.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace PostProcess
{

enum class VolumeShape : uint8_t
{
    Global,   // Always active, weight = 1
    Box,      // AABB — center + half-extents in local/world space
    Sphere,   // center + extents.x as radius
};

// Optional per-stage overrides. A stage with std::nullopt means "leave base
// params alone for this volume". A stage with a populated value is blended
// into the running result by the volume's weight.
struct VolumeOverride
{
    std::optional<CASParams>           cas;
    std::optional<AutoExposureParams>  autoExposure;
    std::optional<BloomParams>         bloom;
    std::optional<TonemappingParams>   tonemapping;

    // True if this volume overrides at least one stage.
    bool HasAnyOverride() const
    {
        return cas.has_value()
            || autoExposure.has_value()
            || bloom.has_value()
            || tonemapping.has_value();
    }
};

struct Volume
{
    VolumeShape shape = VolumeShape::Global;

    // Box: center in world space, extents = half-extents (ignored for Global).
    // Sphere: center = center, extents.x = radius (y/z unused).
    DirectX::XMFLOAT3 center  = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 extents = { 1.0f, 1.0f, 1.0f };

    // Smooth falloff distance outside the shape's boundary (world units).
    // Inside → weight = 1; boundary + blendDistance → weight = 0.
    float blendDistance = 1.0f;

    // Higher priority layers on top (applied later in the blend walk).
    // Ties broken by registration order.
    int priority = 0;

    // Per-frame user-facing toggle. Disabled volumes produce weight = 0 and
    // are skipped entirely by the blender.
    bool enabled = true;

    VolumeOverride override;

    // Optional debug label for Editor UI. Not used by the engine.
    char label[32] = {};
};

// ---------------------------------------------------------------------------
// Shared math helpers — used by every volume source (VolumeSystem, ECS
// EntityVolumeSource, …) so they produce identical falloff for the same
// shape+blendDistance. Kept inline/header-local because they're tiny.
// ---------------------------------------------------------------------------

// Signed distance from @p p to the boundary of @p v. Negative inside,
// positive outside, 0 on the boundary. Global always returns -1.
inline float ComputeSignedDistance(const Volume& v, const DirectX::XMFLOAT3& p)
{
    switch (v.shape)
    {
    case VolumeShape::Global:
        return -1.0f;

    case VolumeShape::Box:
    {
        // AABB SDF: q = |p - c| - e. If every component of q is <= 0 the
        // point is inside; we return the negative max component (distance
        // to nearest face). Outside, the length of max(q,0) is the
        // exterior distance.
        const float dx = std::fabs(p.x - v.center.x) - v.extents.x;
        const float dy = std::fabs(p.y - v.center.y) - v.extents.y;
        const float dz = std::fabs(p.z - v.center.z) - v.extents.z;

        const float ox = std::max(dx, 0.0f);
        const float oy = std::max(dy, 0.0f);
        const float oz = std::max(dz, 0.0f);
        const float outside = std::sqrt(ox*ox + oy*oy + oz*oz);
        const float inside  = std::min(std::max(dx, std::max(dy, dz)), 0.0f);
        return outside + inside;
    }

    case VolumeShape::Sphere:
    {
        const float dx = p.x - v.center.x;
        const float dy = p.y - v.center.y;
        const float dz = p.z - v.center.z;
        const float d  = std::sqrt(dx*dx + dy*dy + dz*dz);
        const float r  = std::max(v.extents.x, 0.0f);
        return d - r;
    }
    }
    return 1.0e9f;
}

// Maps signed distance → [0,1] blend weight with linear falloff over
// @p blendDistance. 1 inside/on boundary; 0 at or beyond blend edge.
inline float ComputeWeightFromDistance(float signedDist, float blendDistance)
{
    if (signedDist <= 0.0f) return 1.0f;
    if (blendDistance <= 0.0f) return 0.0f;          // hard edge
    if (signedDist >= blendDistance) return 0.0f;
    return 1.0f - (signedDist / blendDistance);
}

// ---------------------------------------------------------------------------
// Snapshot — the common language consumed by ParameterBlender. Produced by
// every IVolumeSource (VolumeSystem / EntityVolumeSource / …) and by
// ScriptedOverrideSystem. Lives in Volume.h (rather than a source-side
// header) so multiple unrelated sources can emit it without pulling each
// other's headers.
//
// The pointed-to VolumeOverride must remain valid until the consuming
// frame's blend completes; every producer guarantees that.
// ---------------------------------------------------------------------------
struct Snapshot
{
    float                 weight   = 0.0f;
    int                   priority = 0;
    const VolumeOverride* override = nullptr;
};

} // namespace PostProcess
