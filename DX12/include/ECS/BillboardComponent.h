#pragma once

// BillboardComponent — marks an entity as a camera-facing billboard.
//
// The entity's LocalTransform provides world position.
// The billboard VS ignores the world matrix rotation and reconstructs
// a camera-facing orientation using the camera's right/up vectors.
//
// Routing:
//   BillboardMode::Opaque      → GBufferPass  (deferred, participates in lighting)
//   BillboardMode::Transparent → TransparentPass (forward, alpha blend)
//   BillboardMode::Additive    → TransparentPass (forward, additive blend)
//
// The Renderer checks for BillboardComponent during DrawPacket construction
// and sets the BILLBOARD permutation bit so the VS knows to orient the quad.

#include <cstdint>

enum class BillboardMode : uint8_t
{
    Opaque      = 0,  // GBuffer pass, deferred lit
    AlphaClip   = 1,  // GBuffer pass with alpha test (clip), deferred lit
    Transparent = 2,  // Forward, SRC_ALPHA / INV_SRC_ALPHA
    Additive    = 3,  // Forward, SRC_ALPHA / ONE (particles, glow)
};

struct BillboardComponent
{
    BillboardMode mode       = BillboardMode::Transparent;
    float         worldSize  = 1.0f;    // world-space width/height of the quad
    bool          fixedSize  = false;   // true = constant screen-space size (like gizmo icons)
    float         fixedPixels = 48.0f;  // screen-space size when fixedSize=true
};
