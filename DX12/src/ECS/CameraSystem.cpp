#include "ECS/CameraSystem.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <DirectXMath.h>

using namespace DirectX;

static constexpr float kPitchMax = XM_PIDIV2 - 0.01f;

void CameraSystem::Update(CameraComponent& cam, float dx, float dy, float dt)
{
    cam.yaw   += dx * cam.mouseSensitivity;
    cam.pitch -= dy * cam.mouseSensitivity;  // invert Y: mouse-up → look up
    cam.pitch  = std::clamp(cam.pitch, -kPitchMax, kPitchMax);
    cam.yaw    = std::fmod(cam.yaw, XM_2PI);

    if (dt > 0.0f)
    {
        float speed = cam.moveSpeed * dt;
        if (GetAsyncKeyState(VK_LSHIFT) & 0x8000) speed *= 3.0f;

        const float cp = std::cos(cam.pitch);
        const float sp = std::sin(cam.pitch);
        const float cy = std::cos(cam.yaw);
        const float sy = std::sin(cam.yaw);

        XMVECTOR forward = XMVectorSet(sy * cp, -sp, cy * cp, 0.f);
        XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        XMVECTOR right   = XMVector3Normalize(XMVector3Cross(up, forward));

        XMVECTOR pos = XMLoadFloat3(&cam.position);

        if (GetAsyncKeyState('W') & 0x8000) pos = XMVectorAdd(pos, XMVectorScale(forward, speed));
        if (GetAsyncKeyState('S') & 0x8000) pos = XMVectorSubtract(pos, XMVectorScale(forward, speed));
        if (GetAsyncKeyState('D') & 0x8000) pos = XMVectorAdd(pos, XMVectorScale(right, speed));
        if (GetAsyncKeyState('A') & 0x8000) pos = XMVectorSubtract(pos, XMVectorScale(right, speed));
        if (GetAsyncKeyState('E') & 0x8000) pos = XMVectorAdd(pos, XMVectorScale(up, speed));
        if (GetAsyncKeyState('Q') & 0x8000) pos = XMVectorSubtract(pos, XMVectorScale(up, speed));

        XMStoreFloat3(&cam.position, pos);
    }
}

DirectX::XMMATRIX CameraSystem::BuildViewMatrix(const CameraComponent& cam)
{
    const float cp = std::cos(cam.pitch);
    const float sp = std::sin(cam.pitch);
    const float cy = std::cos(cam.yaw);
    const float sy = std::sin(cam.yaw);

    // LH FPS forward: (sin(yaw)*cos(pitch),  -sin(pitch),  cos(yaw)*cos(pitch))
    const XMVECTOR forward = XMVectorSet(sy * cp, -sp, cy * cp, 0.f);
    const XMVECTOR pos     = XMLoadFloat3(&cam.position);
    const XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);

    return XMMatrixLookToLH(pos, forward, up);
}
