#pragma once

// ReflectionProbeComponent — opt-in marker placed on an entity to register a
// reflection probe in the scene. The Renderer scans the World each frame, packs
// matching entities into a GPU StructuredBuffer<GPUReflectionProbe>, and
// reserves a slice in the per-renderer cubemap-array.
//
// Position comes from the entity's GlobalTransform translation (NOT stored
// here) so probes follow scene-graph parents naturally.
//
// Box convention (parallax + weight) — both AABBs are LOCAL to probe position:
//   innerExtents : half-size of the "full influence" box. Inside → weight 1.
//   outerExtents : half-size of the falloff envelope. Outside → weight 0.
//                  Between inner and outer → linear fade.
// outer must be >= inner per axis; clamped on upload.
//
// cubemapSlice : managed by Renderer. ~0u means "not yet baked / unassigned".
// flags        : bit0 = baked (cubemap content valid), bit1 = needsRebake.

#include <cstdint>
#include <DirectXMath.h>

struct ReflectionProbeComponent
{
    enum FLAGS : uint32_t
    {
        EMPTY        = 0,
        BAKED        = 1u << 0, // cubemap-array slice has valid content
        NEEDS_REBAKE = 1u << 1, // editor / runtime requested a re-capture
    };

    // Half-extents of the inner full-influence box (probe-local, world-aligned).
    DirectX::XMFLOAT3 innerExtents = { 4.0f, 2.0f, 4.0f };
    // Half-extents of the outer falloff box. Must be >= innerExtents per axis.
    DirectX::XMFLOAT3 outerExtents = { 6.0f, 3.0f, 6.0f };

    // Renderer-assigned slice into the cubemap array. ~0u until first bake.
    static constexpr uint32_t kInvalidSlice = ~0u;
    uint32_t cubemapSlice = kInvalidSlice;

    uint32_t flags = EMPTY;

    // ---- Realtime tick (Phase f) ------------------------------------------
    // realtime=true makes the renderer auto-enqueue this probe for re-bake
    // every tickIntervalFrames frames. Default 60 ≈ 1s @ 60fps; raise for
    // distant probes that change slowly, lower for fast time-of-day cycles.
    // lastBakedFrame is runtime state — Renderer stamps it with the current
    // frame index after the bake CL is queued; not serialized.
    bool     realtime           = false;
    uint32_t tickIntervalFrames = 60;
    uint64_t lastBakedFrame     = 0;

    constexpr bool IsBaked() const       { return (flags & BAKED) != 0; }
    constexpr bool NeedsRebake() const   { return (flags & NEEDS_REBAKE) != 0; }
    constexpr void SetBaked(bool v)      { if (v) flags |= BAKED;        else flags &= ~BAKED; }
    constexpr void RequestRebake()       { flags |= NEEDS_REBAKE; }
    constexpr void ClearRebakeRequest()  { flags &= ~NEEDS_REBAKE; }
};
