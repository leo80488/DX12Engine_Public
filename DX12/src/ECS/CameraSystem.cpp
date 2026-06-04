#include "ECS/CameraSystem.h"
#include "ECS/ECS.h"               // World — ResolveFollowing reads target's GlobalTransform
#include "ECS/HierarchyComponents.h"
#include "Input/InputSystem.h"
#include "Physics/PhysicsSystem.h" // CastSphereClosest for camera-collision probe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>     // VK_* constants only (Input handles state)

#include <algorithm>
#include <cmath>
#include <DirectXMath.h>

using namespace DirectX;

static constexpr float kPitchMax = XM_PIDIV2 - 0.01f;

// FPS forward convention (left-handed):
//   forward = (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))
// This is exactly +Z rotated by pitch around X then yaw around Y, i.e.
// XMQuaternionRotationRollPitchYaw(pitch, yaw, 0).
static XMVECTOR ForwardFromYawPitch(float yaw, float pitch)
{
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    return XMVectorSet(sy * cp, -sp, cy * cp, 0.f);
}

void CameraSystem::Update(CameraControllerComponent& ctrl, LocalTransform& xform,
                          float dx, float dy, float dt)
{
    ctrl.yaw   += dx * ctrl.mouseSensitivity;
    ctrl.pitch -= dy * ctrl.mouseSensitivity;  // invert Y: mouse-up → look up
    ctrl.pitch  = std::clamp(ctrl.pitch, -kPitchMax, kPitchMax);
    ctrl.yaw    = std::fmod(ctrl.yaw, XM_2PI);

    // WASD strafe only in Free mode. Player camera modes leave horizontal
    // motion to PlayerControllerSystem; ResolveFollowing later overwrites
    // the position from the target anyway, so any keyboard nudge here would
    // be visible for a single frame and then snap back — confusing for
    // players. Mouse yaw/pitch still applies in all modes.
    if (dt > 0.0f && ctrl.mode == CameraControllerComponent::Mode::Free)
    {
        const Input& in = Input::Get();
        float speed = ctrl.moveSpeed * dt;
        if (in.IsKeyDown(VK_LSHIFT)) speed *= 3.0f;

        const XMVECTOR forward = ForwardFromYawPitch(ctrl.yaw, ctrl.pitch);
        const XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        const XMVECTOR right   = XMVector3Normalize(XMVector3Cross(up, forward));

        XMVECTOR pos = XMLoadFloat3(&xform.translation);

        if (in.IsKeyDown('W')) pos = XMVectorAdd(pos, XMVectorScale(forward, speed));
        if (in.IsKeyDown('S')) pos = XMVectorSubtract(pos, XMVectorScale(forward, speed));
        if (in.IsKeyDown('D')) pos = XMVectorAdd(pos, XMVectorScale(right, speed));
        if (in.IsKeyDown('A')) pos = XMVectorSubtract(pos, XMVectorScale(right, speed));
        if (in.IsKeyDown('E')) pos = XMVectorAdd(pos, XMVectorScale(up, speed));
        if (in.IsKeyDown('Q')) pos = XMVectorSubtract(pos, XMVectorScale(up, speed));

        XMStoreFloat3(&xform.translation, pos);
    }

    // Mirror the controller's yaw/pitch onto the transform's rotation so
    // LocalTransform stays the single source of truth for the camera pose.
    XMStoreFloat4(&xform.rotation,
                  XMQuaternionRotationRollPitchYaw(ctrl.pitch, ctrl.yaw, 0.f));
}

void CameraSystem::SyncControllerFromTransform(CameraControllerComponent& ctrl,
                                               const LocalTransform& xform)
{
    // Recover yaw/pitch from the transform's forward vector. Inverse of the
    // ForwardFromYawPitch / RollPitchYaw mapping above (assumes no roll).
    const XMVECTOR q   = XMLoadFloat4(&xform.rotation);
    const XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0.f, 0.f, 1.f, 0.f), q);
    XMFLOAT3 f;
    XMStoreFloat3(&f, fwd);

    ctrl.pitch = -std::asin(std::clamp(f.y, -1.f, 1.f));
    ctrl.yaw   = std::atan2(f.x, f.z);
}

DirectX::XMMATRIX CameraSystem::BuildViewMatrix(const GlobalTransform& xform)
{
    const XMMATRIX world = XMLoadFloat4x4(&xform.matrix);

    // World-space camera basis: translation = row 3, forward = +Z row.
    const XMVECTOR pos     = world.r[3];
    const XMVECTOR forward = XMVector3Normalize(world.r[2]);
    const XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);

    return XMMatrixLookToLH(pos, forward, up);
}

LocalTransform CameraSystem::MakeTransform(const CameraControllerComponent& ctrl,
                                           const DirectX::XMFLOAT3& position)
{
    LocalTransform lt;
    lt.translation = position;
    XMStoreFloat4(&lt.rotation,
                  XMQuaternionRotationRollPitchYaw(ctrl.pitch, ctrl.yaw, 0.f));
    return lt;
}

void CameraSystem::ResolveFollowing(World& world,
                                    CameraControllerComponent& ctrl,
                                    LocalTransform& xform,
                                    float dt,
                                    DX12Physics::PhysicsSystem* physics)
{
    using Mode = CameraControllerComponent::Mode;
    if (ctrl.mode == Mode::Free) return;
    const Entity followE = ctrl.followTarget.Resolve(world);
    if (followE == NullEntity) return;

    const GlobalTransform* targetGT = world.GetComponent<GlobalTransform>(followE);
    if (!targetGT) return;

    // Decompose target world matrix: we only need the translation. Rotation
    // of the camera comes from ctrl.yaw/pitch, NOT the target — the player
    // looks where the camera looks, not vice-versa.
    XMVECTOR vScale, vQuat, vTrans;
    const XMMATRIX targetM = XMLoadFloat4x4(&targetGT->matrix);
    if (!XMMatrixDecompose(&vScale, &vQuat, &vTrans, targetM)) return;
    XMFLOAT3 targetPos; XMStoreFloat3(&targetPos, vTrans);

    const XMVECTOR yawAxis   = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    const XMVECTOR camFwd    = XMVector3Normalize(
        XMVectorSet(std::sin(ctrl.yaw) * std::cos(ctrl.pitch),
                    -std::sin(ctrl.pitch),
                    std::cos(ctrl.yaw) * std::cos(ctrl.pitch),
                    0.f));
    const XMVECTOR camRight  = XMVector3Normalize(XMVector3Cross(yawAxis, camFwd));

    XMFLOAT3 worldPos;
    if (ctrl.mode == Mode::FirstPerson)
    {
        // Glue camera at target + headOffset (head offset is in world axes
        // for simplicity — a target-local offset would need the target's
        // rotation, which we deliberately ignore). Camera position has no
        // distance term. First-person is intentionally rigid (smoothing the
        // eye would induce lag/nausea); reset the smoothing state so a later
        // switch to ThirdPerson re-snaps instead of gliding from a stale focus.
        worldPos.x = targetPos.x + ctrl.headOffset.x;
        worldPos.y = targetPos.y + ctrl.headOffset.y;
        worldPos.z = targetPos.z + ctrl.headOffset.z;
        ctrl.followSmoothInit = false;
        ctrl.smoothedDistance = 0.f;
    }
    else // ThirdPerson
    {
        // Focus point = target + shoulderOffset (also world-axis aligned).
        // Camera sits behind that point along -forward by thirdPersonDistance,
        // with a lateral shoulder bias along +right so the character isn't
        // dead-centred on the screen.
        const XMVECTOR focusV =
            XMVectorAdd(vTrans, XMVectorSet(0.f, ctrl.shoulderOffset.y, 0.f, 0.f));
        const XMVECTOR sideV  = XMVectorScale(camRight, ctrl.shoulderOffset.x);
        const XMVECTOR desiredFocus = XMVectorAdd(focusV, sideV);

        // --- Frame-rate-independent focus smoothing -----------------------
        // Low-pass the orbit pivot so target jitter / 60 Hz interpolation
        // aliasing doesn't pass straight through to the camera as shake/blur.
        // We smooth the FOCUS (not the final camera position), so changing
        // yaw/pitch re-orbits instantly without rubber-banding the radius.
        // dt<=0 (paused / hit-stop / editor-stopped) snaps so the camera
        // never glides while the sim is frozen. A large per-frame jump snaps
        // too, so respawns / warps don't produce a long glide.
        const bool firstResolve = !ctrl.followSmoothInit;
        XMVECTOR focusWithShoulder = desiredFocus;
        if (!firstResolve)
        {
            const XMVECTOR prevFocus = XMLoadFloat3(&ctrl.smoothedFocus);
            const float jump = XMVectorGetX(
                XMVector3Length(XMVectorSubtract(desiredFocus, prevFocus)));
            constexpr float kTeleportSnap = 25.0f;  // metres in one frame
            if (dt > 0.f && ctrl.followLag > 1e-4f && jump < kTeleportSnap)
            {
                const float a = 1.f - std::exp(-dt / ctrl.followLag);
                focusWithShoulder = XMVectorLerp(prevFocus, desiredFocus, a);
            }
        }
        XMStoreFloat3(&ctrl.smoothedFocus, focusWithShoulder);

        // Spring-arm collision probe. Sweep a sphere from the focus point
        // (player's chest/shoulder) toward the desired camera pose; on hit,
        // clamp the orbit distance so the camera stops short of the wall.
        // No body filter: the CharacterVirtual doesn't register a body in
        // the broadphase by default, so it can't hit itself; if you add an
        // inner body via CCC later, plumb its ID through here.
        float targetDistance = ctrl.thirdPersonDistance;
        if (physics && ctrl.cameraCollisionEnabled && ctrl.thirdPersonDistance > 0.f)
        {
            XMFLOAT3 fromPt; XMStoreFloat3(&fromPt, focusWithShoulder);
            XMFLOAT3 dirF3;  XMStoreFloat3(&dirF3,  XMVectorNegate(camFwd));
            const auto hit = physics->CastSphereClosest(
                fromPt, dirF3,
                std::max(0.01f, ctrl.cameraProbeRadius),
                ctrl.thirdPersonDistance);
            if (hit.hit)
            {
                // Pull in to just-before-the-wall. The shrink keeps the camera
                // outside the wall by the probe radius (Jolt already inset by
                // the sphere radius via mFraction, so we don't double-inset).
                targetDistance = std::max(0.05f, hit.distance);
            }
        }

        // --- Asymmetric distance damping (anti-flicker spring arm) ---------
        // Pull IN instantly (never let the camera sit inside a wall) but ease
        // OUT slowly. Without this, an intermittent probe hit (grazing a
        // corner / ground / railing) toggles the clamp on and off frame-to-
        // frame and punches the camera in and out along the view axis.
        float clampedDistance = targetDistance;
        if (firstResolve || ctrl.smoothedDistance <= 0.f)
        {
            clampedDistance = targetDistance;                 // snap on first frame
        }
        else if (targetDistance < ctrl.smoothedDistance)
        {
            clampedDistance = targetDistance;                 // closer wall: pull in now
        }
        else if (dt > 0.f && ctrl.distanceLag > 1e-4f)
        {
            const float a = 1.f - std::exp(-dt / ctrl.distanceLag);
            clampedDistance = ctrl.smoothedDistance +
                              (targetDistance - ctrl.smoothedDistance) * a;  // ease out
        }
        ctrl.smoothedDistance = clampedDistance;
        ctrl.followSmoothInit = true;

        const XMVECTOR backV  = XMVectorScale(camFwd, -clampedDistance);
        const XMVECTOR posV   = XMVectorAdd(focusWithShoulder, backV);
        XMStoreFloat3(&worldPos, posV);
    }

    xform.translation = worldPos;
    XMStoreFloat4(&xform.rotation,
                  XMQuaternionRotationRollPitchYaw(ctrl.pitch, ctrl.yaw, 0.f));
}
