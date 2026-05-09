#pragma once

// WorldSpaceUISystem — per-frame computation for world-space UI entities.
//
//   * Walks WorldSpaceUIComponent pool
//   * Reads each entity's GlobalTransform (already populated by
//     TransformSystem / FollowSystem upstream)
//   * Computes camera-space depth, distance fade, ConstantWorld scale
//     factor, behind-camera cull flag — writes back into the component's
//     `computedAlpha` / `computedScale` / `isCulled` fields
//   * Drives DamageNumberComponent: integrates velocity, decrements
//     lifetime, destroys spent entities
//
// Pure data-flow system. No GPU work. Render pass reads the computed
// fields and emits geometry.

#include <DirectXMath.h>

class World;

namespace UI
{
    struct WorldSpaceUIView
    {
        DirectX::XMFLOAT4X4 viewProjMatrix; // un-jittered, row-major
        DirectX::XMFLOAT3   cameraRightWS;  // world-space unit right
        DirectX::XMFLOAT3   cameraUpWS;     // world-space unit up
        DirectX::XMFLOAT2   canvasSize{ 1.0f, 1.0f }; // viewport pixels (for ConstantPixel mode)
        bool                valid = false;
    };

    class WorldSpaceUISystem
    {
    public:
        // Update fade / scale / cull for every WorldSpaceUI entity, and
        // advance DamageNumber lifetimes.  Pass dt in seconds.
        void Tick(World& world, const WorldSpaceUIView& view, float dt);
    };

} // namespace UI
