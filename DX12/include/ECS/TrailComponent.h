#pragma once

// TrailComponent — ribbon trail attached to an entity's world position.
//
// Each frame the Renderer snapshots the entity's GlobalTransform translation
// and appends a new segment to a GPU ring buffer (capped at kMaxSegments
// per trail, owned by TrailSystem). A compute shader writes the new sample
// into the correct slot + advances the ring cursor. The render pass
// extrudes each consecutive pair of segments into a camera-facing ribbon
// quad with width and colour that fade along the segment's age.
//
// Trail slot assignment is lazy: the first frame that sees a given entity
// allocates an unused slot in TrailSystem's pool. Slots are leased for the
// component's lifetime; when the entity is destroyed the slot is returned.

#include <cstdint>
#include <DirectXMath.h>

struct TrailComponent
{
    // ---- Visual parameters --------------------------------------------------
    float             width      = 0.15f;  // ribbon half-width at spawn, world units
    float             maxAge     = 1.5f;   // seconds until a segment is fully faded
    DirectX::XMFLOAT4 startColor = { 1.0f, 0.9f, 0.4f, 1.0f }; // head (newest)
    DirectX::XMFLOAT4 endColor   = { 1.0f, 0.3f, 0.1f, 0.0f }; // tail (oldest)

    // ---- Sampling behaviour ------------------------------------------------
    // Only spawn a new segment if the entity has moved more than this
    // distance since the last sample. Prevents a stationary entity from
    // filling the ring with duplicate points.
    float             minSampleDistance = 0.02f;

    // ---- Emitter control --------------------------------------------------
    bool              enabled    = true;

    // ---- CPU-only per-frame state (not serialized) ------------------------
    // Slot index assigned by TrailSystem (0..kMaxTrails-1). 0xFFFFFFFF =
    // unassigned; TrailSystem::CollectTrails allocates on first sight.
    uint32_t          trailSlot        = 0xFFFFFFFFu;
    // Last spawn position, used to gate minSampleDistance.
    DirectX::XMFLOAT3 lastSamplePos    = { 0.0f, 0.0f, 0.0f };
    bool              hasLastSample    = false;
};
