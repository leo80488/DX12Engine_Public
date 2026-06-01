#pragma once

// CameraStackComponents.h — ECS-based virtual camera system.
// See DesignMd/camera_stack_system.md for the architectural rationale.
//
// Data flow:
//   Behavior systems (Follow / Aim / Cinematic) write CameraPoseComponent on
//   each VCam entity. CameraStackSystem manages the per-channel priority list
//   and the per-VCam blend state machine. CameraResolveSystem produces one
//   LiveCameraComponent per channel; the Renderer reads only LiveCameraComponent.

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Channel identification
// ---------------------------------------------------------------------------

// 32-bit hashed channel name. ChannelId{0} == invalid sentinel; the default
// "Main" channel is precomputed via Camera::HashChannel("Main") so scripts
// can pass either the literal hash or a string that the binding resolves.
using CameraChannelId = uint32_t;

namespace Camera
{
    // FNV-1a 32-bit so the hash is stable across runs / serialized data.
    constexpr CameraChannelId HashChannel(const char* s) noexcept
    {
        uint32_t h = 0x811C9DC5u;
        while (*s) { h ^= static_cast<uint8_t>(*s++); h *= 0x01000193u; }
        return h;
    }
    inline constexpr CameraChannelId kMainChannel = HashChannel("Main");
}

// ---------------------------------------------------------------------------
// Per-VCam components
// ---------------------------------------------------------------------------

// VirtualCameraComponent — lens parameters + channel binding for a VCam
// entity. Coexists with the legacy CameraComponent during the migration; the
// CameraStack/Resolve pipeline reads from here, the legacy renderer fallback
// reads from CameraComponent.
struct VirtualCameraComponent
{
    float           fov            = DirectX::XM_PI / 3.f;  // vertical, radians
    float           nearZ          = 0.1f;
    float           farZ           = 200.f;
    float           aspectOverride = 0.f;                   // 0 = inherit from viewport
    CameraChannelId channelId      = Camera::kMainChannel;
};

// CameraPoseComponent — desired world-space pose this VCam wants to be at
// THIS frame. Written by behavior systems (FollowCameraSystem, AimCameraSystem,
// CinematicCameraSystem) BEFORE CameraResolveSystem reads it.
//
// For the legacy FPS camera path, App's RefreshMainCamera bridges
// GlobalTransform → CameraPoseComponent so the existing CameraSystem keeps
// driving the pose without any behavior system attached.
struct CameraPoseComponent
{
    DirectX::XMFLOAT3   position = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4   rotation = { 0.f, 0.f, 0.f, 1.f };  // quaternion
};

// VCamPriorityComponent — stack ordering. Within a channel the highest
// priority wins; ties blend by weight. `enabled=false` means "ignore me"
// (target died, behavior went idle, etc.) without removing the VCam from
// the stack — useful so a lower-priority cam doesn't churn through
// BlendIn/BlendOut every time the higher one disables itself transiently.
struct VCamPriorityComponent
{
    int   priority = 0;
    float weight   = 1.f;
    bool  enabled  = true;
};

// Blend curve types — applied to the normalized 0..1 progress through a
// blend. Custom is for hand-authored curves added later; for now treat it
// as Linear.
enum class BlendCurve : uint8_t
{
    Linear     = 0,
    EaseIn     = 1,
    EaseOut    = 2,
    EaseInOut  = 3,
    Custom     = 4,
};

enum class BlendState : uint8_t
{
    Inactive    = 0,
    BlendingIn  = 1,
    Active      = 2,
    BlendingOut = 3,
};

// VCamBlendComponent — runtime blend state machine. CameraStackSystem
// transitions `state` based on whether this VCam is a winner / loser this
// frame; `currentBlend` is the source of truth for resolve weighting.
//
// `blendInDuration` / `blendOutDuration` of 0 collapses to a hard cut for
// that direction; HardCutTo() sets both to 0 AND signals historyValid=false
// for one frame.
struct VCamBlendComponent
{
    float       blendInDuration  = 0.25f;   // seconds
    float       blendOutDuration = 0.25f;
    BlendCurve  curveIn          = BlendCurve::EaseOut;
    BlendCurve  curveOut         = BlendCurve::EaseIn;
    float       currentBlend     = 0.f;     // 0..1
    BlendState  state            = BlendState::Inactive;
};

// ---------------------------------------------------------------------------
// Behavior components (Phase D)
// ---------------------------------------------------------------------------

struct FollowCameraTag : ComponentBase {};

struct FollowCameraComponent
{
    // Save-stable reference to the follow target. Persists across scene
    // reload via GUID; see DesignMd/entity_persistence_architecture.md.
    AttachmentRef     target;
    DirectX::XMFLOAT3 offset          = { 0.f, 1.6f, -4.f };  // target-local offset to camera
    DirectX::XMFLOAT3 lookAtOffset    = { 0.f, 1.5f,  0.f };  // target-local point to aim at
    float             damping         = 8.f;                  // position lerp factor (per sec)
    float             rotationDamping = 8.f;                  // rotation slerp factor (per sec)
    bool              useLookAt       = true;
};

struct AimCameraTag : ComponentBase {};

struct AimCameraComponent
{
    // Pivot entity (usually the player root). Save-stable via GUID.
    AttachmentRef     target;
    DirectX::XMFLOAT3 pivotOffset  = { 0.f, 1.5f, 0.f }; // target-local pivot offset
    float             yaw          = 0.f;         // radians, driven by input/lua
    float             pitch        = 0.f;
    float             distance     = 3.5f;        // camera back-distance from pivot
    float             pitchMin     = -1.4f;       // ~-80 deg
    float             pitchMax     =  1.0f;       // ~+57 deg
    bool              collisionAvoid = true;
    float             probeRadius  = 0.20f;       // sweep sphere radius
};

// CameraShakeComponent — additive shake layer. Trauma decays each frame;
// CameraShakeNoiseSystem turns trauma² into translation+rotation noise
// applied to the Live pose AFTER resolve. Lives on the LiveCamera entity
// (per channel), not on individual VCams — shake is a property of "the view",
// not of the source.
struct CameraShakeComponent
{
    float             trauma         = 0.f;     // 0..1
    float             falloffPerSec  = 1.5f;    // trauma drain per second
    DirectX::XMFLOAT3 posAmplitude   = { 0.10f, 0.10f, 0.05f };
    DirectX::XMFLOAT3 rotAmplitude   = { 0.04f, 0.04f, 0.04f };
    float             frequency      = 18.f;
    uint32_t          seed           = 0x9E3779B9u;
    float             time           = 0.f;     // running phase accumulator
};

// ---------------------------------------------------------------------------
// Channel-side resolved data
// ---------------------------------------------------------------------------

// CameraStackChannelSingleton — lives on a dedicated channel entity together
// with LiveCameraComponent. CameraStackSystem rebuilds `entries` every frame
// from VirtualCameraComponent + VCamPriorityComponent + VCamBlendComponent;
// CameraResolveSystem reads it. Two separate systems so a channel can be
// resolved without re-sorting (skip-resolve cases) and so the sort can be
// parallelized per channel in the future.
struct CameraStackChannelSingleton
{
    struct StackEntry
    {
        Entity     vcam         = NullEntity;
        int        priority     = 0;
        float      weight       = 1.f;
        float      currentBlend = 0.f;
    };

    CameraChannelId         channelId = Camera::kMainChannel;
    std::vector<StackEntry> entries;          // sorted by priority desc each frame
    bool                    requestHardCut = false; // set by Camera.HardCutTo, consumed by Resolve
};

// LiveCameraComponent — the per-channel resolved camera. RenderSystem, CSM,
// SSR, GTAO and FrustumCulling all read ONLY this component. Built by
// CameraResolveSystem from the stack entries above.
//
// `historyValid=false` for one frame after a hard cut. Temporal passes (TAA,
// XeGTAO, VolumetricFog, SSR) clear their history when they observe a
// false → true transition.
struct LiveCameraComponent
{
    DirectX::XMFLOAT3 position     = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4 rotation     = { 0.f, 0.f, 0.f, 1.f };
    DirectX::XMFLOAT3 forward      = { 0.f, 0.f, 1.f };
    float             fov          = DirectX::XM_PI / 3.f;
    float             nearZ        = 0.1f;
    float             farZ         = 200.f;
    CameraChannelId   channelId    = Camera::kMainChannel;
    bool              historyValid = false;       // false on hard cut / first frame
    bool              hasPrev      = false;       // false until at least one valid frame elapsed
};

// ---------------------------------------------------------------------------
// Inline helpers — used by both the system implementations and Lua bindings
// ---------------------------------------------------------------------------

namespace Camera
{
    // Map a 0..1 progress through a blend curve. Custom collapses to Linear
    // until a custom curve table is wired in.
    inline float ApplyBlendCurve(BlendCurve c, float t) noexcept
    {
        if (t <= 0.f) return 0.f;
        if (t >= 1.f) return 1.f;
        switch (c)
        {
        case BlendCurve::Linear:    return t;
        case BlendCurve::EaseIn:    return t * t;
        case BlendCurve::EaseOut:   return 1.f - (1.f - t) * (1.f - t);
        case BlendCurve::EaseInOut: return t * t * (3.f - 2.f * t);  // smoothstep
        case BlendCurve::Custom:    return t;
        }
        return t;
    }
}
