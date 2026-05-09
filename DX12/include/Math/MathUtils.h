#pragma once
// MathUtils.h — Math utility functions for the DX12 engine.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <DirectXMath.h>
#include <cmath>

namespace MathUtils
{
    // Convert Euler angles (XYZ, degrees) to a quaternion (xyzw).
    // Rotation order: pitch (X) → yaw (Y) → roll (Z).
    inline DirectX::XMFLOAT4 EulerDegreesToQuaternion(DirectX::XMFLOAT3 eulerDeg)
    {
        using namespace DirectX;
        XMVECTOR q = XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(eulerDeg.x),
            XMConvertToRadians(eulerDeg.y),
            XMConvertToRadians(eulerDeg.z));
        XMFLOAT4 result;
        XMStoreFloat4(&result, q);
        return result;
    }

    // Convert a quaternion (xyzw) to Euler angles (XYZ, degrees).
    // Returns pitch (X), yaw (Y), roll (Z) in degrees.
    inline DirectX::XMFLOAT3 QuaternionToEulerDegrees(DirectX::XMFLOAT4 quat)
    {
        using namespace DirectX;
        // Standard ZYX intrinsic → XYZ extrinsic decomposition.
        const float x = quat.x, y = quat.y, z = quat.z, w = quat.w;

        float sinr_cosp = 2.f * (w * x + y * z);
        float cosr_cosp = 1.f - 2.f * (x * x + y * y);
        float pitch = atan2f(sinr_cosp, cosr_cosp);

        float sinp = 2.f * (w * y - z * x);
        float yaw = fabsf(sinp) >= 1.f
            ? copysignf(XM_PIDIV2, sinp)
            : asinf(sinp);

        float siny_cosp = 2.f * (w * z + x * y);
        float cosy_cosp = 1.f - 2.f * (y * y + z * z);
        float roll = atan2f(siny_cosp, cosy_cosp);

        return XMFLOAT3{
            XMConvertToDegrees(pitch),
            XMConvertToDegrees(yaw),
            XMConvertToDegrees(roll)
        };
    }

} // namespace MathUtils
