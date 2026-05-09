#pragma once

// Ray — world-space ray with viewport unprojection and intersection tests.
// Used by EditorLayer for object picking and drag-on-plane.

#include <DirectXMath.h>

struct RenderView;

struct Ray
{
    DirectX::XMFLOAT3 origin;
    DirectX::XMFLOAT3 direction; // unit length

    // Build a ray from a viewport-relative pixel position.
    // pixelX/Y: mouse coords relative to the viewport content's top-left corner.
    // vpW/H:    viewport content size in pixels.
    static Ray FromViewport(float pixelX, float pixelY,
                            float vpW,    float vpH,
                            const RenderView& view);

    // Ray vs oriented bounding box.
    // The AABB is defined in local space (localCenter ± halfExtents) and
    // transformed to world space via worldMatrix.
    // Returns true on hit; outT = ray parameter at the entry point (>= 0).
    bool IntersectsAABB(const DirectX::XMFLOAT3&   localCenter,
                        const DirectX::XMFLOAT3&   halfExtents,
                        const DirectX::XMFLOAT4X4& worldMatrix,
                        float& outT) const;

    // Ray vs infinite plane (planePoint lies on the plane, planeNormal is its normal).
    // Returns true if not parallel and t > 0; outT = ray parameter.
    bool IntersectPlane(const DirectX::XMFLOAT3& planePoint,
                        const DirectX::XMFLOAT3& planeNormal,
                        float& outT) const;

    // Point along the ray at parameter t: origin + t * direction.
    DirectX::XMFLOAT3 At(float t) const;
};
