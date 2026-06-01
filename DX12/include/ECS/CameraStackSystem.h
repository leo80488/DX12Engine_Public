#pragma once

// CameraStackSystem.h — pipeline of systems that drive virtual cameras.
// See DesignMd/camera_stack_system.md.
//
//   PhysicsInterpolation:
//     * FollowCameraTickSystem  — writes CameraPoseComponent for FollowCamera VCams
//     * AimCameraTickSystem     — writes CameraPoseComponent for AimCamera VCams
//   PreRender:
//     * CameraStackTickSystem   — rebuilds per-channel stack + advances blend state
//     * CameraResolveTickSystem — blends VCam poses into LiveCameraComponent
//     * CameraShakeTickSystem   — additive shake on the resolved Live pose
//
// Each system is a thin ISystem adapter; the heavy lifting lives in the .cpp
// as free functions (Camera::AdvanceStack, Camera::Resolve, etc.) so the
// design-doc terminology maps 1:1 onto code.

#include "ECS/ISystem.h"
#include "ECS/TickPhase.h"
#include "ECS/CameraStackComponents.h"

class World;
struct FrameContext;

namespace DX12Physics { class PhysicsSystem; }

namespace Camera
{
    // ---- Channel-singleton management ----------------------------------
    //
    // GetOrCreateChannelEntity returns the entity that carries the
    // CameraStackChannelSingleton + LiveCameraComponent for the requested
    // channel. Creates it (and the singleton + Live component) on first
    // touch. Call from App init for the Main channel; Lua bindings call it
    // implicitly via Camera.PushVCam.
    Entity GetOrCreateChannelEntity(World& world, CameraChannelId channelId);

    Entity FindChannelEntity(World& world, CameraChannelId channelId);

    // ---- Stack manipulation (used by Lua + behavior systems) -----------
    //
    // PushVCam adds (or refreshes) a VCam on the stack. If the entity
    // already has the four core VCam components, only Priority/Blend are
    // patched in (so a re-push during BlendingOut transitions back to
    // BlendingIn from current blend, never from 0).
    struct PushVCamArgs
    {
        int             priority         = 10;
        float           weight           = 1.f;
        float           blendInDuration  = 0.25f;
        float           blendOutDuration = 0.25f;
        BlendCurve      curveIn          = BlendCurve::EaseOut;
        BlendCurve      curveOut         = BlendCurve::EaseIn;
        CameraChannelId channelId        = Camera::kMainChannel;
    };

    void PushVCam(World& world, Entity vcam, const PushVCamArgs& args);

    // PopVCam triggers a BlendingOut transition; the entity itself stays
    // alive. blendOutOverride < 0 = use the entity's existing blendOutDuration.
    void PopVCam(World& world, Entity vcam,
                 float blendOutOverride = -1.f,
                 BlendCurve curveOverride = BlendCurve::Custom);

    // HardCutTo: skip the next blend on this channel and set
    // LiveCameraComponent.historyValid=false for one frame. If `vcam` is
    // non-null, it is pushed with priority just above the current top so the
    // resolve picks it up this frame.
    void HardCutTo(World& world, Entity vcam, CameraChannelId channelId);
}

// ===========================================================================
// System adapters
// ===========================================================================

// FollowCameraTickSystem — drives all FollowCameraTag entities. Reads target
// GlobalTransform, writes own CameraPoseComponent. Lives in
// PhysicsInterpolation (after TransformPropagate so target poses are fresh).
class FollowCameraTickSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "FollowCameraSystem"; }
};

// AimCameraTickSystem — third-person orbit cam with optional spherecast
// wall-collision. Optional PhysicsSystem ref enables the sweep; pass null
// for no collision (e.g. cinematic preview).
class AimCameraTickSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    explicit AimCameraTickSystem(DX12Physics::PhysicsSystem* p = nullptr)
        : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "AimCameraSystem"; }
private:
    DX12Physics::PhysicsSystem* m_physics = nullptr;
};

// CameraStackTickSystem — for each channel: rebuild entries from VCams,
// resolve the winner, advance every VCam's blend state machine. Does NOT
// touch LiveCameraComponent; that's CameraResolveTickSystem's job.
class CameraStackTickSystem
    : public SystemInPhase<TickPhase::PreRender>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "CameraStackSystem"; }
};

// CameraResolveTickSystem — blends contributing VCam poses (currentBlend>0)
// into LiveCameraComponent. Handles prev/curr bookkeeping for historyValid.
class CameraResolveTickSystem
    : public SystemInPhase<TickPhase::PreRender>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "CameraResolveSystem"; }
};

// CameraShakeTickSystem — applies CameraShakeComponent (when present on the
// channel entity) additively on top of the resolved Live pose. Trauma drains
// each frame. Sits after Resolve so shake doesn't double up across blending VCams.
class CameraShakeTickSystem
    : public SystemInPhase<TickPhase::PreRender>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "CameraShakeSystem"; }
};
