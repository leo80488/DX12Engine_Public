#pragma once

// CameraSystem — FPS-style camera controller.
//
// The camera entity owns a CameraControllerComponent (yaw/pitch/tuning) and a
// LocalTransform (its pose). CameraSystem applies viewport mouse deltas + WASD
// to the controller and writes the resulting pose onto the LocalTransform;
// TransformSystem then propagates it to GlobalTransform like any other entity.
//
// Follow modes: when CameraControllerComponent::mode != Free the WASD path
// is skipped (the player controller owns horizontal motion) and the camera's
// world position is instead recomputed each frame from the follow-target's
// pose via ResolveFollowing — call that AFTER TransformSystem::Propagate so
// the target's GlobalTransform is fresh.

#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform, GlobalTransform
#include <DirectXMath.h>

class World;
namespace DX12Physics { class PhysicsSystem; }

class CameraSystem
{
public:
    // Apply a mouse delta (viewport pixels) + WASD movement to the FPS
    // controller. Updates ctrl.yaw/pitch (pitch clamped to ±89°) and writes the
    // resulting translation + rotation onto @p xform. Pass dt > 0 to enable
    // keyboard movement. WASD is suppressed when ctrl.mode != Free.
    void Update(CameraControllerComponent& ctrl, LocalTransform& xform,
                float dx, float dy, float dt = 0.0f);

    // Reposition the camera to track its followTarget. Call AFTER physics +
    // TransformSystem::Propagate so the target's GlobalTransform reflects
    // the current frame's movement. No-op when mode == Free or
    // followTarget is null / dead. Caller is responsible for refreshing the
    // camera's own GlobalTransform after this writes LocalTransform (the
    // camera is typically a root entity, so a simple xform.ToMatrix() write
    // suffices and matches App's existing patch-after-cam-update pattern).
    //
    // @p physics is optional. When non-null AND ctrl.cameraCollisionEnabled,
    // ThirdPerson mode does a sphere sweep from the focus point toward the
    // desired camera pose and clamps `thirdPersonDistance` short of any
    // hit — prevents the camera poking through walls. Pass null for a
    // collision-free preview cam (or set cameraCollisionEnabled = false).
    static void ResolveFollowing(World& world,
                                 const CameraControllerComponent& ctrl,
                                 LocalTransform& xform,
                                 DX12Physics::PhysicsSystem* physics = nullptr);

    // Re-seed the controller's yaw/pitch from an existing LocalTransform
    // rotation. Call when the active camera changes, a world is loaded, or
    // something other than the controller (script, turntable, gizmo) moved the
    // camera — so the next Update() picks up from the current pose without a
    // visible jump.
    static void SyncControllerFromTransform(CameraControllerComponent& ctrl,
                                            const LocalTransform& xform);

    // Build a left-handed view matrix from a world-space camera transform.
    static DirectX::XMMATRIX BuildViewMatrix(const GlobalTransform& xform);

    // Build a LocalTransform whose translation is @p position and whose
    // rotation matches the controller's yaw/pitch. Convenience for scene setup
    // so a freshly-spawned camera entity starts at a well-defined pose.
    static LocalTransform MakeTransform(const CameraControllerComponent& ctrl,
                                        const DirectX::XMFLOAT3& position);
};
