#pragma once

// CameraSystem — FPS-style camera controller.
//
// Call Update() each frame with the viewport mouse delta (right-drag pixels).
// Call BuildViewMatrix() to obtain the DirectX LH view matrix.

#include "ECS/Components.h"
#include <DirectXMath.h>

class CameraSystem
{
public:
    // Apply a mouse delta (viewport pixels) to the camera's yaw/pitch.
    // Pitch is clamped to ±89° to prevent gimbal flip.
    void Update(CameraComponent& cam, float dx, float dy, float dt = 0.0f);

    // Build a left-handed view matrix from the camera's position and orientation.
    static DirectX::XMMATRIX BuildViewMatrix(const CameraComponent& cam);
};
