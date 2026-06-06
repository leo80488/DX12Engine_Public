#pragma once

// ComponentReflection.h — central registry of Reflect::Descriptor<T>
// specializations for every engine component editable via the generic
// reflection-driven inspector.
//
// Conventions:
//   - One REFLECT_BEGIN/END block per type, grouped by header.
//   - Enum option arrays are declared above the block that uses them.
//   - Pure-data components map every editable field via REFLECT_*.
//     Components with hand-written widgets (texture pickers, conditional
//     UI, action buttons) declare only their data fields here and supply
//     the extras via the postDraw callback in RegisterReflectedComponent.
//   - Components whose UI is dominated by custom logic (MaterialComponent,
//     LightData direction picker, RigidBody conditional UI, hierarchy
//     pickers, etc.) intentionally have no descriptor — they keep their
//     original RegisterComponentEditor block.
//
// Including this header from any TU that calls Reflect::Describe<T>() for
// the listed types is sufficient — the specializations live in this one
// file. EditorLayer.cpp is currently the only consumer; if other code
// later needs reflection (serializer codegen, tooling), it can include
// the same file.

#include "Reflection/Reflect.h"
#include "Reflection/ReflectionEditor.h"

// imgui pulled in for the inline custom-widget helpers (DrawQuaternionAsEuler
// / DrawDirectionAsAzEl). ComponentReflection.h is currently only included by
// EditorLayer.cpp which already links imgui.
#include "imgui/imgui.h"

#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/CloudComponent.h"
#include "ECS/TODComponents.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/DDGIComponents.h"
#include "ECS/TrailComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/PhysicsComponents.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/PlayerComponent.h"
#include "ECS/AIIntentComponent.h"
#include "ECS/PerceptionComponent.h"
#include "Nav/NavComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/FootIKComponent.h"
#include "ECS/ParticleComponent.h"
#include "ECS/TerrainComponent.h"
#include "ECS/SocketSystem.h"
#include "ECS/FollowComponents.h"
#include "ECS/VolumeComponent.h"
#include "ECS/VideoComponent.h"
#include "AI/AIComponents.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Scripting/ScriptComponent.h"
#include "UI/UIComponents.h"
#include "UI/UICanvas.h"
#include "UI/WorldSpaceUI.h"

#include <DirectXMath.h>
#include <unordered_map>

// ===========================================================================
// HierarchyComponents — pure data
// ===========================================================================

// VisibilityComponent — author-intent flags + per-view selection mask.
//
// `flags` (uint8) packs Visible / CastShadow / RenderInMainPass; expanded as
// three checkboxes. `viewMask` (uint32) is one bit per ViewBit (main camera,
// each shadow cascade, RT, etc.). Both go through custom widgets — a 4-billion
// drag-int wouldn't let you set individual bits, and the wider one-checkbox-
// per-ViewBit list also doubles as documentation for what bits exist.
namespace Reflect_CustomWidgets
{
    inline bool DrawVisibilityFlags(void* fp, const Reflect::FieldDescriptor& f)
    {
        auto* bits = static_cast<uint8_t*>(fp);
        bool changed = false;
        ImGui::PushID(f.label);
        auto toggle = [&](const char* lbl, uint8_t mask) {
            bool v = (*bits & mask) != 0;
            if (ImGui::Checkbox(lbl, &v)) {
                if (v) *bits |= mask; else *bits &= ~mask;
                changed = true;
            }
        };
        toggle("Visible",            VisibilityComponent::Visible);
        toggle("Cast Shadow",        VisibilityComponent::CastShadow);
        toggle("Render In Main Pass",VisibilityComponent::RenderInMainPass);
        ImGui::PopID();
        return changed;
    }

    inline bool DrawViewMaskBits(void* fp, const Reflect::FieldDescriptor& f)
    {
        auto* mask = static_cast<uint32_t*>(fp);
        bool changed = false;
        ImGui::PushID(f.label);
        ImGui::TextDisabled("View Mask (per-view participation)");

        auto toggle = [&](const char* lbl, uint32_t bit) {
            bool v = (*mask & bit) != 0;
            if (ImGui::Checkbox(lbl, &v)) {
                if (v) *mask |= bit; else *mask &= ~bit;
                changed = true;
            }
        };
        toggle("Main Camera",       ViewBit::MainCamera);
        toggle("Shadow Cascade 0",  ViewBit::ShadowCascade0);
        toggle("Shadow Cascade 1",  ViewBit::ShadowCascade1);
        toggle("Shadow Cascade 2",  ViewBit::ShadowCascade2);
        toggle("Shadow Cascade 3",  ViewBit::ShadowCascade3);
        toggle("Reflection Probe",  ViewBit::ReflectionProbe);
        toggle("Planar Reflection", ViewBit::PlanarReflection);
        toggle("Ray Tracing",       ViewBit::RayTracing);
        toggle("Custom Depth",      ViewBit::CustomDepth);

        // Quick presets cover the common authoring cases.
        if (ImGui::SmallButton("All"))      { *mask = ViewBit::All;        changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("None"))     { *mask = 0u;                  changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Main+Shadow")) {
            *mask = ViewBit::MainCamera | ViewBit::ShadowAny;             changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Shadow Only")) {
            *mask = ViewBit::ShadowAny;                                   changed = true;
        }
        // Hex readout so users editing serialized files can sanity-check.
        ImGui::Text("Raw: 0x%08X", *mask);
        ImGui::PopID();
        return changed;
    }
}

REFLECT_BEGIN(VisibilityComponent)
    REFLECT_CUSTOM(flags,           "Flags",     &Reflect_CustomWidgets::DrawVisibilityFlags)
    REFLECT_CUSTOM(viewMask,        "View Mask", &Reflect_CustomWidgets::DrawViewMaskBits)
    REFLECT_BOOL  (inheritedHidden, "Inherited Hidden (read-only)")
REFLECT_END()

REFLECT_BEGIN(RenderLayer)
    REFLECT_UINT(mask, "Layer Mask", 0u, 0xFFFFFFFFu)
REFLECT_END()

REFLECT_BEGIN(LocalAabb)
    REFLECT_FLOAT3(min, "Min", -1e6f, 1e6f)
    REFLECT_FLOAT3(max, "Max", -1e6f, 1e6f)
REFLECT_END()

// ===========================================================================
// CameraComponent — lens / view-projection parameters
// ===========================================================================

REFLECT_BEGIN(CameraComponent)
    REFLECT_ANGLE (fov,      "FOV",     10.f,   170.f)
    REFLECT_FLOAT_FMT(nearZ, "Near Z",  0.001f, 10.f,   "%.3f", 0.001f)
    REFLECT_FLOAT_FMT(farZ,  "Far Z",   1.f,    5000.f, "%.0f", 1.f)
REFLECT_END()

// ===========================================================================
// CameraControllerComponent — FPS controller state (yaw/pitch + tuning).
// The camera pose itself is edited via the Local Transform component.
// ===========================================================================

constexpr Reflect::EnumOption kCameraModeOptions[] = {
    { (int)CameraControllerComponent::Mode::Free,        "Free (fly-cam)"  },
    { (int)CameraControllerComponent::Mode::ThirdPerson, "Third-Person"    },
    { (int)CameraControllerComponent::Mode::FirstPerson, "First-Person"    },
};

REFLECT_BEGIN(CameraControllerComponent)
    REFLECT_SLIDER(yaw,                 "Yaw (rad)",         -3.14159f, 3.14159f)
    REFLECT_SLIDER(pitch,               "Pitch (rad)",       -1.5f,     1.5f)
    REFLECT_FLOAT_FMT(mouseSensitivity, "Mouse Sensitivity",  0.0001f,  0.05f,  "%.4f", 0.0001f)
    REFLECT_FLOAT_FMT(moveSpeed,        "Move Speed",         0.1f,     1000.f, "%.1f", 0.1f)
    REFLECT_ENUM(mode,                  "Mode",              kCameraModeOptions)
    // Follow-mode fields only relevant when mode != Free.
    REFLECT_IF([](const void* o) {
        return static_cast<const CameraControllerComponent*>(o)->mode
            != CameraControllerComponent::Mode::Free;
    })
        // followTarget is an AttachmentRef — picker UI lives in EditorLayer
        // postDraw (see RegisterDefaultEditors), not in reflection.
        REFLECT_FLOAT(thirdPersonDistance, "Third-Person Distance",   0.1f, 50.f)
        REFLECT_FLOAT3(headOffset,        "FP Head Offset",          -10.f, 10.f)
        REFLECT_FLOAT3(shoulderOffset,    "TP Shoulder Offset",      -10.f, 10.f)
        REFLECT_BOOL  (cameraCollisionEnabled, "TP Wall-Collision Probe")
        REFLECT_FLOAT (cameraProbeRadius, "TP Probe Radius",          0.01f, 1.0f)
        REFLECT_FLOAT_FMT(followLag,      "TP Follow Lag (s, 0=off)", 0.0f, 1.0f, "%.3f", 0.005f)
        REFLECT_FLOAT_FMT(distanceLag,    "TP Distance Lag (s)",      0.0f, 1.0f, "%.3f", 0.005f)
    REFLECT_ENDIF()
REFLECT_END()

// ===========================================================================
// CameraData — generic camera parameters (not user-facing FPS controls)
// ===========================================================================

REFLECT_BEGIN(CameraData)
    REFLECT_ANGLE(fov,                  "FOV",                  10.f,  170.f)
    REFLECT_FLOAT(nearZ,                "Near Z",               0.001f, 10.f)
    REFLECT_FLOAT(farZ,                 "Far Z",                1.f,    10000.f)
    REFLECT_FLOAT(aspectRatioOverride,  "Aspect Override (0=auto)", 0.f, 8.f)
REFLECT_END()

// ===========================================================================
// Camera stack — virtual camera + pose + priority + blend + live
// ===========================================================================

REFLECT_BEGIN(VirtualCameraComponent)
    REFLECT_ANGLE    (fov,            "FOV",             10.f, 170.f)
    REFLECT_FLOAT_FMT(nearZ,          "Near Z",         0.001f, 10.f,   "%.3f", 0.001f)
    REFLECT_FLOAT_FMT(farZ,           "Far Z",          1.f,    5000.f, "%.0f", 1.f)
    REFLECT_FLOAT    (aspectOverride, "Aspect Override (0=auto)", 0.f, 8.f)
    REFLECT_UINT     (channelId,      "Channel Id (hashed)",      0u, 0xFFFFFFFFu)
REFLECT_END()

REFLECT_BEGIN(CameraPoseComponent)
    REFLECT_FLOAT3(position, "Position", -1e5f, 1e5f)
    REFLECT_FLOAT4(rotation, "Rotation (xyzw)", -1.f, 1.f)
REFLECT_END()

REFLECT_BEGIN(VCamPriorityComponent)
    REFLECT_INT  (priority, "Priority",   -1000, 10000)
    REFLECT_FLOAT(weight,   "Weight",     0.f,   1.f)
    REFLECT_BOOL (enabled,  "Enabled")
REFLECT_END()

constexpr Reflect::EnumOption kBlendCurveOptions[] = {
    { (int)BlendCurve::Linear,    "Linear"    },
    { (int)BlendCurve::EaseIn,    "Ease In"   },
    { (int)BlendCurve::EaseOut,   "Ease Out"  },
    { (int)BlendCurve::EaseInOut, "Ease In/Out" },
    { (int)BlendCurve::Custom,    "Custom"    },
};

constexpr Reflect::EnumOption kBlendStateOptions[] = {
    { (int)BlendState::Inactive,    "Inactive"    },
    { (int)BlendState::BlendingIn,  "Blending In" },
    { (int)BlendState::Active,      "Active"      },
    { (int)BlendState::BlendingOut, "Blending Out" },
};

REFLECT_BEGIN(VCamBlendComponent)
    REFLECT_FLOAT(blendInDuration,  "Blend-In Duration (s)",  0.f, 5.f)
    REFLECT_FLOAT(blendOutDuration, "Blend-Out Duration (s)", 0.f, 5.f)
    REFLECT_ENUM (curveIn,          "Curve In",  kBlendCurveOptions)
    REFLECT_ENUM (curveOut,         "Curve Out", kBlendCurveOptions)
    REFLECT_SLIDER(currentBlend,    "Current Blend",  0.f, 1.f)
    REFLECT_ENUM (state,            "State", kBlendStateOptions)
REFLECT_END()

REFLECT_BEGIN(FollowCameraComponent)
    // target is an AttachmentRef — picker UI is provided via the component
    // editor's postDraw closure in EditorLayer::RegisterDefaultEditors.
    REFLECT_FLOAT3(offset,          "Offset (target-local)",  -50.f, 50.f)
    REFLECT_FLOAT3(lookAtOffset,    "LookAt Offset",          -50.f, 50.f)
    REFLECT_FLOAT (damping,         "Position Damping",       0.f, 100.f)
    REFLECT_FLOAT (rotationDamping, "Rotation Damping",       0.f, 100.f)
    REFLECT_BOOL  (useLookAt,       "Use LookAt")
REFLECT_END()

REFLECT_BEGIN(AimCameraComponent)
    // target is an AttachmentRef — picker UI is provided via the component
    // editor's postDraw closure in EditorLayer::RegisterDefaultEditors.
    REFLECT_FLOAT3(pivotOffset,    "Pivot Offset",    -50.f, 50.f)
    REFLECT_SLIDER(yaw,            "Yaw (rad)",       -3.14159f, 3.14159f)
    REFLECT_SLIDER(pitch,          "Pitch (rad)",     -1.5f, 1.5f)
    REFLECT_FLOAT (distance,       "Distance",        0.f, 50.f)
    REFLECT_FLOAT (pitchMin,       "Pitch Min",       -1.5f, 1.5f)
    REFLECT_FLOAT (pitchMax,       "Pitch Max",       -1.5f, 1.5f)
    REFLECT_BOOL  (collisionAvoid, "Wall-Collision Probe")
    REFLECT_FLOAT (probeRadius,    "Probe Radius",    0.01f, 1.f)
REFLECT_END()

REFLECT_BEGIN(CameraShakeComponent)
    REFLECT_SLIDER(trauma,        "Trauma",                  0.f, 1.f)
    REFLECT_FLOAT (falloffPerSec, "Falloff /sec",            0.f, 10.f)
    REFLECT_FLOAT3(posAmplitude,  "Position Amplitude",      0.f, 1.f)
    REFLECT_FLOAT3(rotAmplitude,  "Rotation Amplitude (rad)", 0.f, 0.5f)
    REFLECT_FLOAT (frequency,     "Frequency (Hz-ish)",      0.f, 60.f)
REFLECT_END()

REFLECT_BEGIN(LiveCameraComponent)
    REFLECT_FLOAT3(position,     "Position",         -1e5f, 1e5f)
    REFLECT_FLOAT4(rotation,     "Rotation (xyzw)",  -1.f, 1.f)
    REFLECT_FLOAT3(forward,      "Forward",          -1.f, 1.f)
    REFLECT_ANGLE (fov,          "FOV",              10.f, 170.f)
    REFLECT_FLOAT (nearZ,        "Near Z",           0.001f, 10.f)
    REFLECT_FLOAT (farZ,         "Far Z",            1.f, 10000.f)
    REFLECT_UINT  (channelId,    "Channel Id",       0u, 0xFFFFFFFFu)
    REFLECT_BOOL  (historyValid, "History Valid")
REFLECT_END()

// ===========================================================================
// BillboardComponent
// ===========================================================================

constexpr Reflect::EnumOption kBillboardModeOptions[] = {
    { (int)BillboardMode::Opaque,      "Opaque (GBuffer)"      },
    { (int)BillboardMode::AlphaClip,   "Alpha Clip (GBuffer)"  },
    { (int)BillboardMode::Transparent, "Transparent (Forward)" },
    { (int)BillboardMode::Additive,    "Additive (Forward)"    },
};

REFLECT_BEGIN(BillboardComponent)
    REFLECT_ENUM (mode,        "Mode",            kBillboardModeOptions)
    REFLECT_FLOAT(worldSize,   "World Size",      0.001f, 100.f)
    REFLECT_BOOL (fixedSize,   "Fixed Screen Size")
    REFLECT_FLOAT(fixedPixels, "Pixel Size",      1.f,    512.f)
REFLECT_END()

// ===========================================================================
// SkyboxComponent — 3 paths + a few numeric knobs. GPU handles are runtime
// state shown via postDraw in EditorLayer.
// ===========================================================================

REFLECT_BEGIN(SkyboxComponent)
    REFLECT_STRING(irradiancePath,    "Irradiance Path")
    REFLECT_STRING(radiancePath,      "Radiance Path")
    REFLECT_STRING(skyboxPath,        "Skybox Path")
    REFLECT_UINT  (radianceMipLevels, "Radiance Mips", 1u, 16u)
    REFLECT_FLOAT (iblStrength,       "IBL Strength", 0.f, 8.f)
REFLECT_END()

// ===========================================================================
// AtmosphereComponent — procedural sky / time-of-day / aerial / stars.
// Source of truth; Renderer pushes these into SkyIBLPass each frame.
// ===========================================================================

constexpr Reflect::EnumOption kSkyboxSourceOptions[] = {
    { (int)AtmosphereComponent::SkyboxSource::Atmosphere, "Atmosphere (procedural)" },
    { (int)AtmosphereComponent::SkyboxSource::Static,     "Static Cubemap"          },
};

REFLECT_BEGIN(AtmosphereComponent)
    REFLECT_BOOL  (atmosphereEnabled,      "Procedural Atmosphere")
    REFLECT_ENUM  (skyboxSource,           "Skybox Source", kSkyboxSourceOptions)
    REFLECT_HEADER("IBL")
    REFLECT_SLIDER(iblStrength,            "IBL Intensity", 0.f, 3.f)
    REFLECT_INFO  ("Master scale on indirect lighting (diffuse + specular). 0 = no IBL.")
    REFLECT_HEADER("Aerial Perspective")
    REFLECT_BOOL  (aerialCompositeEnabled, "Composite Aerial Perspective")
    REFLECT_INFO  ("Distance fog. Depends on world-unit-to-km scale.")
    REFLECT_HEADER("Stars")
    REFLECT_SLIDER(starDensity,            "Star Density",     1.f,   2048.f)
    REFLECT_SLIDER(starBrightness,         "Star Brightness",  0.f,   2.f)
REFLECT_END()

// ===========================================================================
// TODConfigComponent — author-set Time-of-Day parameters (singleton).
// ===========================================================================
REFLECT_BEGIN(TODConfigComponent)
    REFLECT_BOOL  (enabled,            "Enable Time-of-Day")
    REFLECT_SLIDER(timeOfDay,          "Time (0=midnight, 0.5=noon)", 0.f, 1.f)
    REFLECT_SLIDER(timeSpeed,          "Speed (day/sec)",             0.f, 0.5f)
    REFLECT_SLIDER(latitudeRad,        "Latitude (rad)",             -1.55f, 1.55f)
    REFLECT_SLIDER(sunBrightnessScale, "Sun Brightness",  0.f, 5.f)
    REFLECT_SLIDER(moonIntensityScale, "Moon Intensity",  0.f, 1.f)
REFLECT_END()

// ===========================================================================
// TODOutputComponent — computed each frame; shown read-only as diagnostics.
// ===========================================================================
REFLECT_BEGIN(TODOutputComponent)
    REFLECT_INFO  ("Computed by TODEvaluationSystem. Read-only diagnostics.")
    REFLECT_FLOAT3(activeDirection, "Active Light Dir", -1.f, 1.f)
    REFLECT_COLOR3(activeColor,     "Active Light Color")
    REFLECT_FLOAT3(sunDirection,    "Sun Dir (geom)",   -1.f, 1.f)
    REFLECT_COLOR3(sunColor,        "Sun Color (geom)")
    REFLECT_FLOAT3(moonDirection,   "Moon Dir",         -1.f, 1.f)
    REFLECT_COLOR3(moonColor,       "Moon Disk Color")
    REFLECT_BOOL  (isMoonActive,    "Moon Drives Lighting")
    REFLECT_BOOL  (moonDiskVisible, "Moon Disk Visible")
REFLECT_END()

// ===========================================================================
// SunLightTag / MoonLightTag — empty marker components. Attach to a
// DirectionalLight entity to bring it under TOD control.
// ===========================================================================
REFLECT_BEGIN(SunLightTag)
    REFLECT_INFO("TOD will drive this directional light as the SUN.")
REFLECT_END()

REFLECT_BEGIN(MoonLightTag)
    REFLECT_INFO("TOD will drive this directional light as the MOON.")
REFLECT_END()

// ===========================================================================
// CloudComponent — volumetric cloud authoring knobs (singleton).
// Sun direction + colour come from TODOutputComponent at runtime.
// ===========================================================================
REFLECT_BEGIN(CloudComponent)
    REFLECT_BOOL  (enabled,         "Enabled")
    REFLECT_HEADER("Layer Altitude")
    REFLECT_FLOAT (bottomAltitude,  "Bottom (m)", 0.f,   20000.f)
    REFLECT_FLOAT (topAltitude,     "Top (m)",    0.f,   20000.f)
    REFLECT_HEADER("Shape")
    REFLECT_SLIDER(coverage,        "Coverage",   0.f,   1.f)
    REFLECT_SLIDER(density,         "Density",    0.f,   4.f)
    REFLECT_FLOAT_FMT(noiseScale,   "Noise Scale", 0.0001f, 0.01f, "%.5f", 0.0001f)
    REFLECT_HEADER("Wind")
    REFLECT_FLOAT3(windDirection,   "Wind Direction", -1.f, 1.f)
    REFLECT_SLIDER(windSpeed,       "Wind Speed (m/s)", 0.f, 100.f)
    REFLECT_HEADER("Lighting")
    REFLECT_SLIDER(anisotropy,      "Anisotropy (HG g)", -0.99f, 0.99f)
    REFLECT_SLIDER(extinction,      "Extinction",     0.f, 0.5f)
    REFLECT_SLIDER(ambientStrength, "Ambient Fill",   0.f, 2.f)
    REFLECT_COLOR3(cloudColor,      "Cloud Albedo")
REFLECT_END()

// ===========================================================================
// VideoComponent — playback + surface params. Play / Pause / Stop / Restart
// buttons + seek slider live in EditorLayer's postDraw because they need to
// call Video::* helpers (decoupled from the descriptor).
// ===========================================================================

constexpr Reflect::EnumOption kVideoStateOptions[] = {
    { (int)VideoPlaybackState::Stopped, "Stopped" },
    { (int)VideoPlaybackState::Playing, "Playing" },
    { (int)VideoPlaybackState::Paused,  "Paused"  },
};

constexpr Reflect::EnumOption kVideoCodecOptions[] = {
    { (int)RHI::VideoCodec::H264, "H.264" },
    { (int)RHI::VideoCodec::H265, "H.265" },
};

constexpr Reflect::EnumOption kVideoColorSpaceOptions[] = {
    { 0, "BT.709 (HD)" },
    { 1, "BT.601 (SD)" },
};

REFLECT_BEGIN(VideoComponent)
    REFLECT_HEADER("Playback")
    REFLECT_ENUM  (state,        "State",           kVideoStateOptions)
    REFLECT_SLIDER(playRate,     "Speed",            0.0f, 4.0f)
    REFLECT_BOOL  (loop,         "Loop")
    REFLECT_HEADER("Surface")
    REFLECT_BOOL  (worldSpace,   "World-Space Quad")
    REFLECT_FLOAT (worldWidth,   "World Width (m)",  0.01f, 100.f)
    REFLECT_FLOAT (worldHeight,  "World Height (m)", 0.01f, 100.f)
    REFLECT_SLIDER(rectUMin,     "Screen U Min",     0.f, 1.f)
    REFLECT_SLIDER(rectVMin,     "Screen V Min",     0.f, 1.f)
    REFLECT_SLIDER(rectUMax,     "Screen U Max",     0.f, 1.f)
    REFLECT_SLIDER(rectVMax,     "Screen V Max",     0.f, 1.f)
    REFLECT_SLIDER(renderAlpha,  "Alpha",            0.f, 1.f)
    REFLECT_ENUM  (colorSpace,   "Color Space",      kVideoColorSpaceOptions)
    REFLECT_HEADER("Stream Config (only used by hand-rolled D3D12 decoder path)")
    REFLECT_ENUM  (codec,        "Codec",            kVideoCodecOptions)
    REFLECT_SLIDER_INT(width,    "Max Width",        16, 7680)
    REFLECT_SLIDER_INT(height,   "Max Height",       16, 4320)
REFLECT_END()

// ===========================================================================
// ReflectionProbeComponent — data fields only. The "Bake This Probe" button
// + status text live in EditorLayer's postDraw because they need access to
// Renderer.
// ===========================================================================

REFLECT_BEGIN(ReflectionProbeComponent)
    REFLECT_INFO   ("Position follows entity GlobalTransform.")
    REFLECT_FLOAT3 (innerExtents,       "Inner Half-Extents", 0.01f, 1000.f)
    REFLECT_FLOAT3 (outerExtents,       "Outer Half-Extents", 0.01f, 1000.f)
    REFLECT_INFO   ("Inside inner box: full influence (weight 1).")
    REFLECT_INFO   ("Between inner and outer: linear fade.")
    REFLECT_HEADER ("Realtime")
    REFLECT_BOOL   (realtime,           "Realtime")
    REFLECT_SLIDER_INT(tickIntervalFrames, "Tick Interval (frames)", 1, 3600)
REFLECT_END()

// ===========================================================================
// LightData — VolumetricLightComponent (sibling, simple)
// ===========================================================================

REFLECT_BEGIN(VolumetricLightComponent)
    REFLECT_BOOL  (enabled,        "Enabled")
    REFLECT_SLIDER(intensityScale, "Intensity Scale", 0.f, 10.f)
    REFLECT_INFO  ("Boost only the in-fog brightness; surface lit")
    REFLECT_INFO  ("intensity remains the LightData value.")
REFLECT_END()

// ===========================================================================
// DDGIVolumeComponent — most fields are reflected. The DXR readiness badge
// + dynamic "total probes" warning live in EditorLayer's postDraw because
// they read live Renderer state.
// ===========================================================================

REFLECT_BEGIN(DDGIVolumeComponent)
    REFLECT_HEADER ("Volume")
    REFLECT_FLOAT3 (origin,            "Origin",             -10000.f, 10000.f)
    REFLECT_FLOAT3 (extent,            "Half-Extent",        0.1f,     1000.f)
    REFLECT_HEADER ("Probe Grid")
    REFLECT_UINT   (probeCountsX,      "Probes X",           4u, 32u)
    REFLECT_UINT   (probeCountsY,      "Probes Y",           4u, 32u)
    REFLECT_UINT   (probeCountsZ,      "Probes Z",           4u, 32u)
    REFLECT_SLIDER_INT(raysPerProbe,   "Rays / Probe",       32, 512)
    REFLECT_INFO   ("64 = perf, 128 = default, 256 = quality.")
    REFLECT_HEADER ("Sampling")
    REFLECT_FLOAT  (hysteresis,        "Hysteresis",         0.5f,   0.999f)
    REFLECT_FLOAT  (rotationJitterScale, "Ray Jitter Scale", 0.0f,   1.0f)
    REFLECT_INFO   ("Lower = steadier sun lighting (less flicker on hard shadows), slight bias. ~0.3-0.5 for stable directional GI.")
    REFLECT_FLOAT  (normalBias,        "Normal Bias",        0.f,    2.f)
    REFLECT_FLOAT  (viewBias,          "View Bias",          0.f,    2.f)
    REFLECT_FLOAT  (boundaryFadeRatio, "Boundary Fade",      0.01f,  0.5f)
    REFLECT_HEADER ("Tinting")
    REFLECT_COLOR3 (diffuseTint,       "Diffuse Tint")
    REFLECT_FLOAT  (diffuseScale,      "Diffuse Scale",      0.f,    4.f)
    REFLECT_HEADER ("Phase 3 (toggles persisted; runtime impact pending)")
    REFLECT_BOOL   (enableRelocation,     "Probe Relocation")
    REFLECT_BOOL   (enableClassification, "Probe Classification")
    REFLECT_HEADER ("Debug")
    REFLECT_BOOL   (debugDraw,         "Debug Draw")
REFLECT_END()

// ===========================================================================
// IndirectLightingSettingsComponent — scene-singleton knobs
// ===========================================================================

REFLECT_BEGIN(IndirectLightingSettingsComponent)
    REFLECT_BOOL  (ddgiEnabled,           "DDGI Enabled")
    REFLECT_FLOAT (ddgiDiffuseScale,      "DDGI Diffuse Scale",   0.f, 8.f)
    REFLECT_FLOAT (skyIBLDiffuseScale,    "Sky IBL Diffuse Scale", 0.f, 4.f)
    // 0 = no AO on DDGI (trust probe visibility), 1 = full AO (legacy double-occlude),
    // ~0.4 = recommended. Sky-fallback path always gets full AO.
    REFLECT_FLOAT (ddgiAONearFieldStrength, "DDGI Near-Field AO",  0.f, 1.f)
    REFLECT_HEADER("SSR")
    REFLECT_BOOL  (ssrEnabled,            "SSR Enabled")
    REFLECT_FLOAT (ssrRoughnessCutoff,    "SSR Roughness Cutoff", 0.f, 1.f)
    REFLECT_FLOAT (ssrEdgeFadeRatio,      "SSR Edge Fade",        0.f, 0.5f)
    REFLECT_HEADER("Fallback Order")
    REFLECT_BOOL  (reflectionProbePriorityOverDDGI, "Reflection Probe > DDGI (specular)")
    REFLECT_BOOL  (useDDGIForRoughSpecularFallback, "Use DDGI for Rough Specular Fallback")
REFLECT_END()

// ===========================================================================
// TrailComponent — ribbon trail. trailSlot/lastSamplePos/hasLastSample are
// runtime state; only the visual + sampling parameters are exposed.
// ===========================================================================

REFLECT_BEGIN(TrailComponent)
    REFLECT_BOOL  (enabled,           "Enabled")
    REFLECT_FLOAT (width,             "Width",                0.001f, 5.f)
    REFLECT_FLOAT (maxAge,            "Max Age (s)",          0.05f,  30.f)
    REFLECT_FLOAT (minSampleDistance, "Min Sample Distance",  0.f,    5.f)
    REFLECT_COLOR4(startColor,        "Start Color (head)")
    REFLECT_COLOR4(endColor,          "End Color (tail)")
REFLECT_END()

// ===========================================================================
// Custom widget helpers — referenced by REFLECT_CUSTOM(...) below. Inline
// because only EditorLayer.cpp includes this header (single TU).
// ===========================================================================

namespace Reflect_CustomWidgets
{
    // Quaternion → Euler degrees with a per-component cache so dragging the
    // sliders stays smooth instead of re-deriving the angles every frame
    // (which would lose precision near gimbal-lock-adjacent orientations).
    inline bool DrawQuaternionAsEuler(void* fp, const Reflect::FieldDescriptor& f)
    {
        // imgui.h pulled in transitively via EditorLayer.h.
        struct Cache { DirectX::XMFLOAT4 q; DirectX::XMFLOAT3 e; };
        static std::unordered_map<void*, Cache> s_cache;
        auto* q = static_cast<DirectX::XMFLOAT4*>(fp);
        Cache& c = s_cache[fp];
        if (q->x != c.q.x || q->y != c.q.y || q->z != c.q.z || q->w != c.q.w)
        {
            // Convert quaternion to YXZ Euler degrees. Matches the engine's
            // existing convention used by RegisterComponentEditor<LocalTransform>.
            using namespace DirectX;
            const XMVECTOR qv = XMLoadFloat4(q);
            const XMMATRIX m  = XMMatrixRotationQuaternion(qv);
            // Extract Euler (Y, X, Z order). Same math as the original
            // MathUtils::QuaternionToEulerDegrees helper inlined here so this
            // header has no extra dependency.
            const float sy = -m.r[2].m128_f32[0];
            const float clamped = sy > 0.999999f ? 0.999999f : (sy < -0.999999f ? -0.999999f : sy);
            const float ex = asinf(clamped);
            float ey, ez;
            if (fabsf(clamped) > 0.999999f) {
                ey = atan2f(-m.r[0].m128_f32[1], m.r[1].m128_f32[1]);
                ez = 0.f;
            } else {
                ey = atan2f(m.r[2].m128_f32[1], m.r[2].m128_f32[2]);
                ez = atan2f(m.r[1].m128_f32[0], m.r[0].m128_f32[0]);
            }
            c.e.x = XMConvertToDegrees(ex);
            c.e.y = XMConvertToDegrees(ey);
            c.e.z = XMConvertToDegrees(ez);
            c.q = *q;
        }
        if (ImGui::DragFloat3(f.label, &c.e.x, 0.5f))
        {
            using namespace DirectX;
            const XMVECTOR qv = XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(c.e.x),
                XMConvertToRadians(c.e.y),
                XMConvertToRadians(c.e.z));
            XMStoreFloat4(q, qv);
            c.q = *q;
            return true;
        }
        return false;
    }

    // Light direction picker — store XMFLOAT3 unit vector, edit as
    // azimuth/elevation degrees with a per-component cache.
    inline bool DrawDirectionAsAzEl(void* fp, const Reflect::FieldDescriptor& f)
    {
        struct Cache { DirectX::XMFLOAT3 dir; float az; float el; };
        static std::unordered_map<void*, Cache> s_cache;
        auto* dir = static_cast<DirectX::XMFLOAT3*>(fp);
        Cache& c = s_cache[fp];
        if (dir->x != c.dir.x || dir->y != c.dir.y || dir->z != c.dir.z)
        {
            const float ny = (dir->y < -1.f ? -1.f : (dir->y > 1.f ? 1.f : -dir->y));
            c.el  = DirectX::XMConvertToDegrees(asinf(ny));
            c.az  = DirectX::XMConvertToDegrees(atan2f(-dir->x, -dir->z));
            c.dir = *dir;
        }
        bool changed = false;
        ImGui::PushID(f.label);
        changed |= ImGui::SliderFloat("Azimuth",   &c.az, -180.f, 180.f, "%.1f deg");
        changed |= ImGui::SliderFloat("Elevation", &c.el,  -90.f,  90.f, "%.1f deg");
        ImGui::PopID();
        if (changed)
        {
            const float az = DirectX::XMConvertToRadians(c.az);
            const float el = DirectX::XMConvertToRadians(c.el);
            const float cosEl = cosf(el);
            *dir = { -cosEl * sinf(az), -sinf(el), -cosEl * cosf(az) };
            c.dir = *dir;
        }
        return changed;
    }
}

// ===========================================================================
// LocalTransform — TRS, rotation edited as Euler via the cache above.
// ===========================================================================

REFLECT_BEGIN(LocalTransform)
    REFLECT_FLOAT3(translation, "Translation",  -1e6f, 1e6f)
    REFLECT_CUSTOM(rotation,    "Rotation",
                   &Reflect_CustomWidgets::DrawQuaternionAsEuler)
    REFLECT_FLOAT3(scale,       "Scale",        0.001f, 1000.f)
REFLECT_END()

// ===========================================================================
// LightData — type drives which fields are visible. direction uses the
// azimuth/elevation custom widget for the orientation-friendly UX.
// ===========================================================================

constexpr Reflect::EnumOption kLightTypeOptions[] = {
    { (int)LightType::Directional, "Directional" },
    { (int)LightType::Point,       "Point"       },
    { (int)LightType::Spot,        "Spot"        },
};

REFLECT_BEGIN(LightData)
    REFLECT_ENUM (type,         "Type",         kLightTypeOptions)
    REFLECT_INFO ("Position: use LocalTransform (not stored in Light)")
    // Direction picker — shown for Directional + Spot only.
    REFLECT_IF([](const void* o) {
        const auto* l = static_cast<const LightData*>(o);
        return l->type == LightType::Directional || l->type == LightType::Spot;
    })
        REFLECT_CUSTOM(direction, "Direction",
                       &Reflect_CustomWidgets::DrawDirectionAsAzEl)
    REFLECT_ENDIF()
    // Radius — Point / Spot only (directional is infinite).
    REFLECT_IF([](const void* o) {
        const auto* l = static_cast<const LightData*>(o);
        return l->type == LightType::Point || l->type == LightType::Spot;
    })
        REFLECT_FLOAT(radius, "Radius", 0.1f, 500.f)
    REFLECT_ENDIF()
    // Cone angle — Spot only.
    REFLECT_IF([](const void* o) {
        return static_cast<const LightData*>(o)->type == LightType::Spot;
    })
        REFLECT_ANGLE(spotAngle, "Cone Angle", 1.f, 90.f)
    REFLECT_ENDIF()
    REFLECT_HEADER ("Shading")
    REFLECT_COLOR3 (color,     "Color")
    REFLECT_FLOAT  (intensity, "Intensity", 0.f, 1000.f)
    // Cast shadow — Spot only (directional uses CSM unconditionally; point
    // shadows aren't implemented).
    REFLECT_IF([](const void* o) {
        return static_cast<const LightData*>(o)->type == LightType::Spot;
    })
        REFLECT_BOOL(castsShadow, "Cast Shadow")
    REFLECT_ENDIF()
REFLECT_END()

// ===========================================================================
// MeshHandle — primitive picker (legacy authoring path).
// ===========================================================================

constexpr Reflect::EnumOption kPrimitiveMeshOptions[] = {
    { 0, "Cube"   },
    { 1, "Sphere" },
    { 2, "Cone"   },
};

REFLECT_BEGIN(MeshHandle)
    REFLECT_ENUM(gpuMeshID, "Primitive", kPrimitiveMeshOptions)
REFLECT_END()

// ===========================================================================
// PhysicsComponents — RigidBody (motion-conditional mass), Collider (shape-
// conditional extents).
// ===========================================================================

constexpr Reflect::EnumOption kRigidBodyMotionOptions[] = {
    { (int)RigidBodyComponent::Motion::Static,    "Static"    },
    { (int)RigidBodyComponent::Motion::Kinematic, "Kinematic" },
    { (int)RigidBodyComponent::Motion::Dynamic,   "Dynamic"   },
};

namespace Reflect_CustomWidgets
{
    inline bool DrawRigidBodyAxisLocks(void* fp, const Reflect::FieldDescriptor& f)
    {
        auto* bits = static_cast<uint8_t*>(fp);
        bool changed = false;
        ImGui::PushID(f.label);
        ImGui::TextDisabled("Axis Lock (freezes velocity on the locked axis)");

        auto toggle = [&](const char* lbl, uint8_t mask) {
            bool v = (*bits & mask) != 0;
            if (ImGui::Checkbox(lbl, &v)) {
                if (v) *bits |= mask; else *bits &= ~mask;
                changed = true;
            }
        };

        ImGui::Text("Freeze Position:");
        ImGui::SameLine();
        toggle("X##pos",   RigidBodyComponent::LockTranslationX); ImGui::SameLine();
        toggle("Y##pos",   RigidBodyComponent::LockTranslationY); ImGui::SameLine();
        toggle("Z##pos",   RigidBodyComponent::LockTranslationZ);

        ImGui::Text("Freeze Rotation:");
        ImGui::SameLine();
        toggle("X##rot",   RigidBodyComponent::LockRotationX); ImGui::SameLine();
        toggle("Y##rot",   RigidBodyComponent::LockRotationY); ImGui::SameLine();
        toggle("Z##rot",   RigidBodyComponent::LockRotationZ);

        if (ImGui::SmallButton("None"))         { *bits = 0;                                                   changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Lock Rot"))     { *bits |= RigidBodyComponent::LockRotationAll;                changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Plane 2D (XY)")){ *bits  = RigidBodyComponent::LockTranslationZ
                                                          | RigidBodyComponent::LockRotationX
                                                          | RigidBodyComponent::LockRotationY;                 changed = true; }
        ImGui::PopID();
        return changed;
    }
}

REFLECT_BEGIN(RigidBodyComponent)
    REFLECT_ENUM (motion,         "Motion",          kRigidBodyMotionOptions)
    REFLECT_IF([](const void* o) {
        return static_cast<const RigidBodyComponent*>(o)->motion
            == RigidBodyComponent::Motion::Dynamic;
    })
        REFLECT_FLOAT_FMT(mass, "Mass (kg)", 0.001f, 10000.f, "%.3f", 0.1f)
    REFLECT_ENDIF()
    REFLECT_FLOAT_FMT(linearDamping,  "Linear Damping",  0.f,  1.f, "%.3f", 0.005f)
    REFLECT_FLOAT_FMT(angularDamping, "Angular Damping", 0.f,  1.f, "%.3f", 0.005f)
    REFLECT_FLOAT_FMT(friction,       "Friction",        0.f,  2.f, "%.2f", 0.01f)
    REFLECT_FLOAT_FMT(restitution,    "Restitution",     0.f,  1.f, "%.2f", 0.01f)
    REFLECT_FLOAT_FMT(gravityFactor,  "Gravity Factor", -2.f,  5.f, "%.2f", 0.05f)
    // Axis locks only have effect on Dynamic — hide on Static/Kinematic so
    // users don't toggle inert checkboxes wondering why nothing happened.
    REFLECT_IF([](const void* o) {
        return static_cast<const RigidBodyComponent*>(o)->motion
            == RigidBodyComponent::Motion::Dynamic;
    })
        REFLECT_CUSTOM(lockedAxes, "Constraints",
                       &Reflect_CustomWidgets::DrawRigidBodyAxisLocks)
    REFLECT_ENDIF()
REFLECT_END()

constexpr Reflect::EnumOption kColliderShapeOptions[] = {
    { (int)ColliderComponent::Shape::Box,     "Box"     },
    { (int)ColliderComponent::Shape::Sphere,  "Sphere"  },
    { (int)ColliderComponent::Shape::Capsule, "Capsule" },
    { (int)ColliderComponent::Shape::Mesh,    "Mesh"    },
};

// CharacterControllerComponent — KCC tuning. Runtime state (velocity,
// isGrounded, groundEntity, timeInAir, bodyId, generation) is intentionally
// not exposed: it's overwritten every physics step, so editing it from the
// Inspector would just flicker.
REFLECT_BEGIN(CharacterControllerComponent)
    REFLECT_FLOAT_FMT(capsuleRadius,     "Capsule Radius",      0.05f, 5.0f, "%.3f", 0.01f)
    REFLECT_FLOAT_FMT(capsuleHalfHeight, "Capsule Half-Height", 0.05f, 5.0f, "%.3f", 0.01f)
    REFLECT_FLOAT_FMT(stepHeight,        "Step Height",         0.0f,  2.0f, "%.3f", 0.01f)
    REFLECT_ANGLE    (maxSlopeRad,       "Max Walkable Slope",  0.f,   80.f)
    REFLECT_FLOAT_FMT(skinWidth,         "Skin Width",          0.0f,  0.1f, "%.4f", 0.001f)
    REFLECT_FLOAT_FMT(mass,              "Mass (kg)",           1.0f,  500.f,"%.1f", 0.5f)
    REFLECT_FLOAT_FMT(pushStrength,      "Push Strength",       0.0f,  4.0f, "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(gravity,           "Gameplay Gravity",    -80.f, 0.f,  "%.2f", 0.5f)
    REFLECT_FLOAT_FMT(maxFallSpeed,      "Max Fall Speed",      1.0f,  200.f,"%.1f", 1.0f)
    REFLECT_FLOAT_FMT(jumpSpeed,         "Jump Speed",          0.0f,  30.f, "%.2f", 0.1f)
REFLECT_END()

// PlayerComponent — high-level avatar tuning. cameraEntity is an
// AttachmentRef — picker UI is in EditorLayer postDraw, not reflection.
// jumpBufferTimer is runtime-only.
REFLECT_BEGIN(PlayerComponent)
    REFLECT_FLOAT_FMT(walkSpeed,      "Walk Speed",       0.0f, 50.f, "%.2f", 0.1f)
    REFLECT_FLOAT_FMT(runSpeed,       "Run Speed",        0.0f, 50.f, "%.2f", 0.1f)
    REFLECT_SLIDER   (airControl,     "Air Control",      0.0f,  1.0f)
    REFLECT_FLOAT_FMT(turnRate,       "Turn Rate (rad/s)",0.0f, 40.f, "%.2f", 0.25f)
    REFLECT_FLOAT_FMT(facingMinSpeed, "Facing Min Speed", 0.0f,  5.0f,"%.2f", 0.05f)
    REFLECT_FLOAT_FMT(coyoteTime,     "Coyote Time (s)",  0.0f,  0.5f, "%.3f", 0.005f)
    REFLECT_FLOAT_FMT(jumpBufferTime, "Jump Buffer (s)",  0.0f,  0.5f, "%.3f", 0.005f)
REFLECT_END()

REFLECT_BEGIN(FootIKComponent)
    REFLECT_BOOL  (enabled,        "Enabled")
    REFLECT_SLIDER(enableWeight,   "Weight",        0.0f, 1.0f)
    REFLECT_FLOAT (rayUp,          "Ray Up",        0.0f, 5.0f)
    REFLECT_FLOAT (rayDown,        "Ray Down",      0.0f, 5.0f)
    REFLECT_FLOAT (footOffset,     "Foot Offset",  -1.0f, 1.0f)
    REFLECT_BOOL  (alignToNormal,  "Align To Normal (Phase 2)")
    REFLECT_BOOL  (pelvisDrop,     "Pelvis Drop (Phase 3)")
    // Chain indices are runtime-resolved; show as int so authors can override
    // when auto-detect picks the wrong pair on a non-standard rig. -1 = auto.
    REFLECT_INT   (leftChainIdx,   "Left Chain Idx",  -1, 128)
    REFLECT_INT   (rightChainIdx,  "Right Chain Idx", -1, 128)
REFLECT_END()

REFLECT_BEGIN(AIIntentComponent)
    // Strategic-layer intent. BehaviorTreeSystem writes; AITacticalSystem
    // translates into NavAgent.destination + facingMode. See
    // DesignMd §2.3 for the full goal taxonomy.
    REFLECT_INT   (currentGoal,   "Current Goal", 0, 7)   // enum AIGoal (Idle…UseObject)
    REFLECT_FLOAT3(goalPosition,  "Goal Position", -1.0e6f, 1.0e6f)
    REFLECT_FLOAT (goalPriority,  "Goal Priority", 0.0f, 100.0f)
    REFLECT_FLOAT (goalStartTime, "Goal Start (debug)", 0.0f, 1.0e6f)
REFLECT_END()

REFLECT_BEGIN(NavAgentComponent)
    // Tactical-layer intent + steering tunables. AITacticalSystem writes
    // destination + facingMode, NavAgentSystem reads them and pushes
    // desired velocity onto CharacterControllerComponent.
    REFLECT_BOOL  (hasDestination, "Has Destination")
    REFLECT_FLOAT3(destination,    "Destination", -1.0e6f, 1.0e6f)
    REFLECT_BOOL  (useLookAt,      "Use Look-At")
    REFLECT_FLOAT3(lookTarget,     "Look Target", -1.0e6f, 1.0e6f)
    REFLECT_INT   (facingMode,     "Facing Mode (0=Move,1=Target,2=Manual)", 0, 2)
    // Steering tunables.
    REFLECT_FLOAT (speed,          "Speed",              0.0f,  100.0f)
    REFLECT_FLOAT (arriveRadius,   "Arrive Radius",      0.01f,  10.0f)
    REFLECT_FLOAT (slowdownRadius, "Slowdown Radius",    0.0f,   20.0f)
    REFLECT_FLOAT (repathDistance, "Repath Distance",    0.0f,  100.0f)
    REFLECT_BOOL  (rotateToFacing, "Rotate To Facing")
    REFLECT_FLOAT (turnRate,       "Turn Rate (rad/s)",  0.0f,   50.0f)
REFLECT_END()

REFLECT_BEGIN(PerceptionComponent)
    // Sensing tunables. PerceptionSystem (not yet implemented) will read
    // these to drive visibility raycasts and sound aggregation.
    REFLECT_FLOAT (sightRange,   "Sight Range",      0.0f, 200.0f)
    REFLECT_FLOAT (sightConeDeg, "Sight Cone (deg)", 0.0f, 180.0f)
    REFLECT_FLOAT (hearingRange, "Hearing Range",    0.0f, 100.0f)
    REFLECT_FLOAT (audibleFor,   "Sound Memory (s)", 0.0f, 60.0f)
REFLECT_END()

namespace Reflect_CustomWidgets
{
    // Offset editor with a "Snap to Bottom" preset that pushes the shape so
    // its lowest point touches the entity pivot (= floor for foot-pivoted
    // characters). Per-shape Y formula:
    //   Capsule: halfHeight + radius
    //   Sphere : radius
    //   Box    : halfExtents.y
    //   Mesh   : 0 (mesh authoring decides its own pivot)
    inline bool DrawColliderOffset(void* fp, const Reflect::FieldDescriptor& /*f*/)
    {
        auto* offset = static_cast<DirectX::XMFLOAT3*>(fp);
        // Recover the owning ColliderComponent so the preset reads sibling
        // fields and so we can render both Offset + Rotation in this single
        // custom widget block (one widget per REFLECT_CUSTOM site).
        auto* owner  = reinterpret_cast<ColliderComponent*>(
            reinterpret_cast<std::uint8_t*>(offset) - offsetof(ColliderComponent, offset));
        bool changed = false;
        ImGui::PushID("ColliderOffset");
        changed |= ImGui::DragFloat3("Offset", &offset->x, 0.01f, -1000.f, 1000.f, "%.3f");
        if (ImGui::SmallButton("Snap to Bottom"))
        {
            float y = 0.f;
            switch (owner->shape)
            {
                case ColliderComponent::Shape::Capsule: y = owner->halfHeight + owner->radius; break;
                case ColliderComponent::Shape::Sphere:  y = owner->radius;                     break;
                case ColliderComponent::Shape::Box:     y = owner->halfExtents.y;              break;
                case ColliderComponent::Shape::Mesh:    y = 0.f;                               break;
            }
            offset->x = 0.f;
            offset->y = y;
            offset->z = 0.f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Offset")) { *offset = {0.f, 0.f, 0.f}; changed = true; }

        changed |= ImGui::DragFloat3("Rotation (deg)", &owner->rotationEulerDeg.x,
                                     0.5f, -360.f, 360.f, "%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Rot")) { owner->rotationEulerDeg = {0.f, 0.f, 0.f}; changed = true; }
        // Capsule-on-its-side preset — capsules are Y-axis primitives in
        // Jolt, so 90° about Z lays it along X. Most-asked workflow.
        ImGui::SameLine();
        if (ImGui::SmallButton("Lay X")) { owner->rotationEulerDeg = {0.f, 0.f, 90.f};  changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Lay Z")) { owner->rotationEulerDeg = {90.f, 0.f, 0.f};  changed = true; }

        ImGui::PopID();
        return changed;
    }
}

REFLECT_BEGIN(ColliderComponent)
    REFLECT_ENUM (shape, "Shape", kColliderShapeOptions)
    REFLECT_IF([](const void* o) {
        return static_cast<const ColliderComponent*>(o)->shape
            == ColliderComponent::Shape::Box;
    })
        REFLECT_FLOAT3(halfExtents, "Half Extents", 0.001f, 1000.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        const auto* c = static_cast<const ColliderComponent*>(o);
        return c->shape == ColliderComponent::Shape::Sphere
            || c->shape == ColliderComponent::Shape::Capsule;
    })
        REFLECT_FLOAT(radius, "Radius", 0.001f, 1000.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ColliderComponent*>(o)->shape
            == ColliderComponent::Shape::Capsule;
    })
        REFLECT_FLOAT(halfHeight, "Half-Height (excl. caps)", 0.001f, 1000.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ColliderComponent*>(o)->shape
            == ColliderComponent::Shape::Mesh;
    })
        REFLECT_STRING(meshLibPath,   "Mesh Lib")
        REFLECT_UINT  (meshLibMeshId, "Mesh ID", 0u, 65535u)
    REFLECT_ENDIF()
    REFLECT_CUSTOM(offset, "Offset", &Reflect_CustomWidgets::DrawColliderOffset)
REFLECT_END()

// ===========================================================================
// ChainPhysicsComponent — pure sliders grouped by collapsing headers.
// ===========================================================================

REFLECT_BEGIN(ChainPhysicsComponent)
    REFLECT_BOOL(enabled, "Enabled")
    REFLECT_COLLAPSE("Hair")
        REFLECT_SLIDER   (damping,        "Damping",          0.f, 1.f)
        REFLECT_FLOAT_FMT(gravity,        "Gravity",       -100.f, 0.f, "%.1f", 0.5f)
        REFLECT_SLIDER   (stiffness,      "Stiffness",        0.1f, 5.f)
        REFLECT_SLIDER_INT(iterations,    "Iterations",       1, 20)
        REFLECT_SLIDER_INT(substeps,      "Substeps",         1, 8)
        REFLECT_SLIDER   (maxVelocity,    "Max Velocity",     0.f, 5.f)
        REFLECT_SLIDER   (localStiffness, "Local Stiffness",  0.f, 1.f)
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Skirt")
        REFLECT_SLIDER   (skirtDamping,        "Damping",         0.f, 1.f)
        REFLECT_FLOAT_FMT(skirtGravity,        "Gravity",      -100.f, 0.f, "%.1f", 0.5f)
        REFLECT_SLIDER   (skirtStiffness,      "Stiffness",       0.1f, 5.f)
        REFLECT_SLIDER   (skirtMaxVelocity,    "Max Velocity",    0.f, 5.f)
        REFLECT_SLIDER   (skirtLocalStiffness, "Local Stiffness", 0.f, 1.f)
        REFLECT_SLIDER   (skirtHorizDecay,     "Horiz Decay",     0.f, 1.f)
        REFLECT_INFO     ("Decay: 0=tip no horiz, 1=uniform")
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Spring Bones (Root)")
        REFLECT_FLOAT_FMT(springStiffness, "Stiffness",  10.f, 300.f, "%.1f", 1.f)
        REFLECT_FLOAT_FMT(springDamping,   "Damping",     1.f,  30.f, "%.1f", 0.5f)
        REFLECT_FLOAT_FMT(springMass,      "Mass",        0.1f, 10.f, "%.2f", 0.1f)
        REFLECT_FLOAT_FMT(springGravity,   "Gravity",   -50.f,  0.f,  "%.1f", 0.5f)
        REFLECT_FLOAT_FMT(springMaxDisp,   "Max Disp",    0.1f, 10.f, "%.2f", 0.1f)
        REFLECT_INFO     ("Virtual root (Chest_Root) + independent bones")
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Spring Bones (Child)")
        REFLECT_FLOAT_FMT(springChildStiffness, "Stiffness",  50.f, 500.f, "%.1f", 1.f)
        REFLECT_FLOAT_FMT(springChildDamping,   "Damping",     1.f,  40.f, "%.1f", 0.5f)
        REFLECT_FLOAT_FMT(springChildMass,      "Mass",        0.1f,  5.f, "%.2f", 0.1f)
        REFLECT_FLOAT_FMT(springChildGravity,   "Gravity",   -50.f,  0.f,  "%.1f", 0.5f)
        REFLECT_FLOAT_FMT(springChildMaxDisp,   "Max Disp",    0.1f,  5.f, "%.2f", 0.1f)
        REFLECT_INFO     ("Chest L/R relative to virtual root")
    REFLECT_COLLAPSE_END()
REFLECT_END()

// ===========================================================================
// ScriptComponent — Lua path with .lua drag-drop.
// ===========================================================================

// ScriptComponent now holds a std::vector<ScriptInstance> (multiple scripts per
// entity). A static field descriptor can't express a dynamic list, so the auto
// UI is empty — the entire inspector (per-slot path / enabled / exposed vars /
// add / remove) is hand-drawn in EditorLayer's custom postDraw, mirroring how
// SocketComponent's dynamic list is handled.
REFLECT_BEGIN(ScriptComponent)
REFLECT_END()

// ===========================================================================
// AnimationComponent — paused/looping/speed reflected. Clip combo is engine-
// context-heavy (needs ClipLibrary lookup), kept in postDraw.
// ===========================================================================

REFLECT_BEGIN(AnimationComponent)
    REFLECT_BOOL    (paused,  "Paused")
    REFLECT_BOOL    (looping, "Looping")
    REFLECT_FLOAT_FMT(speed,  "Speed", -4.f, 4.f, "%.2f x", 0.01f)
REFLECT_END()

// ===========================================================================
// MorphComponent — paused/looping reflected. Clip combo + per-channel weight
// editor stay in postDraw (need MorphClipLibrary).
// ===========================================================================

REFLECT_BEGIN(MorphComponent)
    REFLECT_BOOL(paused,  "Paused")
    REFLECT_BOOL(looping, "Looping")
REFLECT_END()

// ===========================================================================
// Display-only components — empty descriptors so RegisterReflectedComponent
// compiles, with the read-only display routed entirely through postDraw.
// ===========================================================================

REFLECT_BEGIN(SkeletonRef)
REFLECT_END()

REFLECT_BEGIN(SkeletonComponent)
REFLECT_END()

REFLECT_BEGIN(CapsuleColliderComponent)
REFLECT_END()

REFLECT_BEGIN(SocketComponent)
REFLECT_END()

REFLECT_BEGIN(FollowEntityComponent)
REFLECT_END()

// ===========================================================================
// UI — UIRootComponent (one entity per UI screen)
// ===========================================================================
// `root` is a unique_ptr<Widget> — not editable from inspector, but the
// other fields drive when/whether the root renders.
REFLECT_BEGIN(UI::UIRootComponent)
    REFLECT_STRING(name,         "Name")
    REFLECT_INT   (sortOrder,    "Sort Order",   -100, 100)
    REFLECT_BOOL  (visible,      "Visible")
    REFLECT_BOOL  (inputEnabled, "Input Enabled")
    REFLECT_FLOAT2(canvasSizeOverride, "Canvas Size Override (0=viewport)", 0.f, 8192.f)
REFLECT_END()

// Screen-space marker. Fields control where flat-ECS UI primitives
// (UIImageComponent / UITextComponent on the same entity) land. Ignored
// for widget-tree UIs — those use their root's LayoutSpec.
REFLECT_BEGIN(UI::UIScreenSpaceComponent)
    REFLECT_SLIDER(anchorX, "Anchor X", 0.f, 1.f)
    REFLECT_SLIDER(anchorY, "Anchor Y", 0.f, 1.f)
    REFLECT_FLOAT (offsetX, "Offset X (px)", -4096.f, 4096.f)
    REFLECT_FLOAT (offsetY, "Offset Y (px)", -4096.f, 4096.f)
    REFLECT_SLIDER(pivotX,  "Pivot X",  0.f, 1.f)
    REFLECT_SLIDER(pivotY,  "Pivot Y",  0.f, 1.f)
REFLECT_END()

// UV addressing modes shared by both UI image components.
constexpr Reflect::EnumOption kUIWrapModeOptions[] = {
    { (int)UI::UIWrapMode::Clamp,  "Clamp" },
    { (int)UI::UIWrapMode::Wrap,   "Wrap (tile)" },
    { (int)UI::UIWrapMode::Mirror, "Mirror" },
};

// Flat-ECS UI image — single textured quad without a Widget tree.
REFLECT_BEGIN(UI::UIImageComponent)
    REFLECT_BOOL  (visible, "Visible")
    REFLECT_STRING_DROP(texturePath, "Texture", "ITEX_PATH")
    REFLECT_FLOAT (sizeX,   "Size X (px)", 1.f, 4096.f)
    REFLECT_FLOAT (sizeY,   "Size Y (px)", 1.f, 4096.f)
    REFLECT_SLIDER(uv0X,    "UV0 X",       0.f, 4.f)
    REFLECT_SLIDER(uv0Y,    "UV0 Y",       0.f, 4.f)
    REFLECT_SLIDER(uv1X,    "UV1 X",       0.f, 4.f)
    REFLECT_SLIDER(uv1Y,    "UV1 Y",       0.f, 4.f)
    REFLECT_ENUM  (wrapMode,    "UV Wrap",      kUIWrapModeOptions)
    REFLECT_BOOL  (pointFilter, "Point Filter")
    REFLECT_COLOR4(tint,    "Tint")
REFLECT_END()

// Flat-ECS UI text — single string. Inspector edits the `text` field live.
constexpr Reflect::EnumOption kTextEffectOptions[] = {
    { (int)UI::TextEffectPreset::None,          "None" },
    { (int)UI::TextEffectPreset::Shadow,        "Drop Shadow" },
    { (int)UI::TextEffectPreset::Outline,       "Outline" },
    { (int)UI::TextEffectPreset::Glow,          "Glow" },
    { (int)UI::TextEffectPreset::Jitter,        "Jitter" },
    { (int)UI::TextEffectPreset::OutlineShadow, "Outline + Shadow" },
    { (int)UI::TextEffectPreset::GlowShadow,    "Glow + Shadow" },
};
REFLECT_BEGIN(UI::UITextComponent)
    REFLECT_BOOL  (visible, "Visible")
    REFLECT_STRING(text,    "Text")
    REFLECT_COLOR4(color,   "Color")
    REFLECT_SLIDER(scale,   "Scale",       0.1f, 8.f)
    REFLECT_ENUM  (effectPreset, "Effect", kTextEffectOptions)
    REFLECT_COLOR4(outlineColor, "Outline Color")
    REFLECT_FLOAT (outlineWidth, "Outline Width (px)", 0.f, 16.f)
    REFLECT_COLOR4(glowColor,    "Glow Color")
    REFLECT_FLOAT (glowWidth,    "Glow Width (px)",    0.f, 32.f)
    REFLECT_COLOR4(shadowColor,  "Shadow Color")
    REFLECT_FLOAT (shadowOffsetX, "Shadow Offset X (px)", -32.f, 32.f)
    REFLECT_FLOAT (shadowOffsetY, "Shadow Offset Y (px)", -32.f, 32.f)
    REFLECT_FLOAT (jitterAmplitude, "Jitter Amplitude (px)", 0.f, 16.f)
    REFLECT_FLOAT (jitterFrequency, "Jitter Frequency (Hz)", 0.f, 60.f)
REFLECT_END()

// Flat-ECS HP / progress bar — bg + filled portion + border. No Widget tree.
REFLECT_BEGIN(UI::UIBarComponent)
    REFLECT_BOOL  (visible,         "Visible")
    REFLECT_SLIDER(value,           "Value (0..1)", 0.f, 1.f)
    REFLECT_FLOAT (sizeX,           "Size X (px)",  1.f, 4096.f)
    REFLECT_FLOAT (sizeY,           "Size Y (px)",  1.f, 4096.f)
    REFLECT_FLOAT (borderThick,     "Border Thickness", 0.f, 16.f)
    REFLECT_COLOR4(fillColor,       "Fill Color")
    REFLECT_COLOR4(backgroundColor, "Background Color")
    REFLECT_COLOR4(borderColor,     "Border Color")
REFLECT_END()

// ===== Entity-as-widget Canvas UI (UI/UICanvas.h) =========================
constexpr Reflect::EnumOption kCanvasRenderModeOptions[] = {
    { (int)UI::CanvasRenderMode::ScreenSpaceOverlay, "Screen Space - Overlay" },
};
constexpr Reflect::EnumOption kCanvasScaleModeOptions[] = {
    { (int)UI::CanvasScaleMode::ConstantPixelSize,   "Constant Pixel Size" },
    { (int)UI::CanvasScaleMode::ScaleWithScreenSize, "Scale With Screen Size" },
};
REFLECT_BEGIN(UI::UICanvas)
    REFLECT_ENUM  (renderMode,          "Render Mode",          kCanvasRenderModeOptions)
    REFLECT_ENUM  (scaleMode,           "Scale Mode",           kCanvasScaleModeOptions)
    REFLECT_FLOAT2(referenceResolution, "Reference Resolution", 1.f, 8192.f)
    REFLECT_SLIDER(matchWidthOrHeight,  "Match Width<->Height", 0.f, 1.f)
    REFLECT_INT   (sortOrder,           "Sort Order",           -1000, 1000)
REFLECT_END()

REFLECT_BEGIN(UI::UIRect)
    REFLECT_FLOAT2(anchorMin, "Anchor Min",  0.f, 1.f)
    REFLECT_FLOAT2(anchorMax, "Anchor Max",  0.f, 1.f)
    REFLECT_FLOAT2(pivot,     "Pivot",       0.f, 1.f)
    REFLECT_FLOAT2(size,      "Size (px)",   -8192.f, 8192.f)
    REFLECT_FLOAT2(offset,    "Offset (px)", -8192.f, 8192.f)
REFLECT_END()

REFLECT_BEGIN(UI::UIImage)
    REFLECT_BOOL  (visible,     "Visible")
    REFLECT_STRING_DROP(texturePath, "Texture", "ITEX_PATH")
    REFLECT_COLOR4(color,       "Color")
    REFLECT_FLOAT2(uv0,         "UV0", 0.f, 4.f)
    REFLECT_FLOAT2(uv1,         "UV1", 0.f, 4.f)
    REFLECT_ENUM  (wrapMode,    "UV Wrap",      kUIWrapModeOptions)
    REFLECT_BOOL  (pointFilter, "Point Filter")
REFLECT_END()

// Sprite-sheet (atlas) animation — drives the entity's UIImage uv0/uv1.
REFLECT_BEGIN(UI::UISpriteAnimComponent)
    REFLECT_BOOL (playing,    "Playing")
    REFLECT_BOOL (loop,       "Loop")
    REFLECT_BOOL (pingpong,   "Ping-Pong")
    REFLECT_INT  (columns,    "Columns",            1, 64)
    REFLECT_INT  (rows,       "Rows",               1, 64)
    REFLECT_INT  (frameCount, "Frame Count (0=all)", 0, 4096)
    REFLECT_FLOAT(fps,        "Frames / sec",        0.f, 120.f)
REFLECT_END()

constexpr Reflect::EnumOption kUITextAlignHOptions[] = {
    { 0, "Left" }, { 1, "Center" }, { 2, "Right" },
};
constexpr Reflect::EnumOption kUITextAlignVOptions[] = {
    { 0, "Top" }, { 1, "Middle" }, { 2, "Bottom" },
};
REFLECT_BEGIN(UI::UIText)
    REFLECT_BOOL  (visible,   "Visible")
    REFLECT_STRING(text,      "Text")
    REFLECT_COLOR4(color,     "Color")
    REFLECT_SLIDER(fontScale, "Font Scale", 0.1f, 8.f)
    REFLECT_ENUM  (alignH,    "Align H", kUITextAlignHOptions)
    REFLECT_ENUM  (alignV,    "Align V", kUITextAlignVOptions)
    REFLECT_ENUM  (effectPreset,   "Effect", kTextEffectOptions)
    REFLECT_COLOR4(outlineColor,   "Outline Color")
    REFLECT_FLOAT (outlineWidth,   "Outline Width (px)", 0.f, 16.f)
    REFLECT_COLOR4(glowColor,      "Glow Color")
    REFLECT_FLOAT (glowWidth,      "Glow Width (px)",    0.f, 32.f)
    REFLECT_COLOR4(shadowColor,    "Shadow Color")
    REFLECT_FLOAT (shadowOffsetX,  "Shadow Offset X (px)", -32.f, 32.f)
    REFLECT_FLOAT (shadowOffsetY,  "Shadow Offset Y (px)", -32.f, 32.f)
    REFLECT_FLOAT (jitterAmplitude,"Jitter Amplitude (px)", 0.f, 16.f)
    REFLECT_FLOAT (jitterFrequency,"Jitter Frequency (Hz)", 0.f, 60.f)
REFLECT_END()

REFLECT_BEGIN(UI::UIInteractable)
    REFLECT_BOOL  (raycastTarget,  "Raycast Target")
    REFLECT_BOOL  (disabled,       "Disabled")
    REFLECT_BOOL  (tintTransition, "Tint On State")
    REFLECT_COLOR4(normalColor,    "Normal Color")
    REFLECT_COLOR4(hoverColor,     "Hover Color")
    REFLECT_COLOR4(pressedColor,   "Pressed Color")
    REFLECT_COLOR4(disabledColor,  "Disabled Color")
REFLECT_END()

// World-space UI — billboarded 3D quads (HP bars, name plates, damage
// numbers) rendered by WorldUIBillboardPass.  Pairs with the entity's
// GlobalTransform — drive position via LocalTransform / FollowEntity.
constexpr Reflect::EnumOption kWorldUIScalingOptions[] = {
    { (int)UI::ScalingMode::ConstantPixel, "Constant Pixel (HUD-on-3D)" },
    { (int)UI::ScalingMode::ConstantWorld, "Constant World (size in metres)" },
};
constexpr Reflect::EnumOption kWorldUIDepthOptions[] = {
    { (int)UI::DepthMode::Always,       "Always (overlay)" },
    { (int)UI::DepthMode::Test,         "Depth Test"       },
    { (int)UI::DepthMode::TestWithFade, "Depth Test + Fade (TBD)" },
};

REFLECT_BEGIN(UI::WorldSpaceUIComponent)
    REFLECT_FLOAT2(baseSize,          "Base Size (px or m)",       0.001f, 1e4f)
    REFLECT_FLOAT2(pivot,             "Pivot",                     0.f,    1.f)
    REFLECT_FLOAT3(screenSpaceOffset, "Screen-Space Offset",       -1e3f,  1e3f)
    REFLECT_ENUM (scalingMode,        "Scaling Mode",              kWorldUIScalingOptions)
    REFLECT_FLOAT(fadeNear,           "Fade Near (m)",             0.f,    1e3f)
    REFLECT_FLOAT(fadeFar,            "Fade Far (m)",              0.f,    1e4f)
    REFLECT_BOOL (hideWhenBehindCamera, "Hide When Behind Camera")
    REFLECT_ENUM (depthMode,          "Depth Mode",                kWorldUIDepthOptions)
REFLECT_END()

REFLECT_BEGIN(UI::WorldUIBarComponent)
    REFLECT_BOOL  (visible,     "Visible")
    REFLECT_SLIDER(value,       "Value (0..1)", 0.f, 1.f)
    REFLECT_FLOAT (borderThick, "Border Thickness", 0.f, 1.f)
    REFLECT_COLOR4(fillColor,       "Fill Color")
    REFLECT_COLOR4(backgroundColor, "Background Color")
    REFLECT_COLOR4(borderColor,     "Border Color")
REFLECT_END()

REFLECT_BEGIN(UI::WorldUITextComponent)
    REFLECT_BOOL  (visible, "Visible")
    REFLECT_STRING(text,    "Text")
    REFLECT_COLOR4(color,   "Color")
    REFLECT_SLIDER(scale,   "Scale", 0.1f, 8.f)
REFLECT_END()

REFLECT_BEGIN(UI::WorldUIImageComponent)
    REFLECT_BOOL  (visible, "Visible")
    REFLECT_COLOR4(tint,    "Tint")
    REFLECT_FLOAT2(uv0,     "UV0", 0.f, 1.f)
    REFLECT_FLOAT2(uv1,     "UV1", 0.f, 1.f)
REFLECT_END()

REFLECT_BEGIN(UI::DamageNumberComponent)
    REFLECT_STRING(text,         "Text")
    REFLECT_COLOR4(color,        "Color")
    REFLECT_FLOAT (lifetime,      "Lifetime Remaining (s)", 0.f,  60.f)
    REFLECT_FLOAT (totalLifetime, "Total Lifetime (s)",     0.01f, 60.f)
    REFLECT_FLOAT3(velocity,      "Velocity (m/s)",         -50.f, 50.f)
REFLECT_END()

REFLECT_BEGIN(FollowSocketComponent)
REFLECT_END()

REFLECT_BEGIN(DecalComponent)
REFLECT_END()

REFLECT_BEGIN(TerrainComponent)
REFLECT_END()

REFLECT_BEGIN(BlackboardComponent)
REFLECT_END()

// ===========================================================================
// BeamComponent — global params reflected. controlPoints is a vector<T> of
// reflected BeamControlPoint records, so the generic vector drawer handles
// add/remove/reorder.
// ===========================================================================

REFLECT_BEGIN(BeamControlPoint)
    REFLECT_FLOAT3(position,  "Position",  -1e6f, 1e6f)
    REFLECT_FLOAT (radius,    "Radius",    0.001f, 5.f)
    REFLECT_COLOR4(colorTint, "Color Tint")
REFLECT_END()

REFLECT_BEGIN(BeamComponent)
    REFLECT_FLOAT (globalRadiusScale, "Radius Scale",     0.001f, 10.f)
    REFLECT_FLOAT (wobbleAmplitude,   "Wobble Amplitude", 0.f,    0.5f)
    REFLECT_FLOAT (wobbleSpeed,       "Wobble Speed",     0.f,    20.f)
    REFLECT_VECTOR(controlPoints,     "Control Points",   BeamControlPoint)
REFLECT_END()

// ===========================================================================
// ParticleEmitterComponent — every shape param uses a visibility predicate
// gated on the shape enum. Texture path opts in to ITEX_PATH drag-drop.
// ===========================================================================

constexpr Reflect::EnumOption kParticleBlendOptions[] = {
    { (int)ParticleBlendMode::Alpha,    "Alpha"    },
    { (int)ParticleBlendMode::Additive, "Additive" },
};

constexpr Reflect::EnumOption kParticleVisualOptions[] = {
    { (int)ParticleVisualMode::Flat,     "Flat"     },
    { (int)ParticleVisualMode::Fire,     "Fire"     },
    { (int)ParticleVisualMode::Smoke,    "Smoke"    },
    { (int)ParticleVisualMode::Electric, "Electric" },
};

constexpr Reflect::EnumOption kParticleShapeOptions[] = {
    { (int)ParticleShape::Point,  "Point"  },
    { (int)ParticleShape::Sphere, "Sphere" },
    { (int)ParticleShape::Cone,   "Cone"   },
    { (int)ParticleShape::Box,    "Box"    },
    { (int)ParticleShape::Circle, "Circle" },
    { (int)ParticleShape::Mesh,   "Mesh"   },
};

REFLECT_BEGIN(ParticleEmitterComponent)
    REFLECT_BOOL     (enabled,        "Enabled")
    REFLECT_FLOAT_FMT(spawnRate,      "Spawn Rate (/s)",  0.f, 10000.f, "%.1f", 1.f)
    REFLECT_FLOAT_FMT(startLifetime,  "Start Lifetime",   0.05f, 30.f,  "%.2f", 0.05f)
    REFLECT_FLOAT    (startSize,      "Start Size",       0.001f, 10.f)
    REFLECT_HEADER   ("Velocity")
    REFLECT_FLOAT3   (velocityMin,    "Velocity Min",    -1e3f, 1e3f)
    REFLECT_FLOAT3   (velocityMax,    "Velocity Max",    -1e3f, 1e3f)
    REFLECT_FLOAT3   (gravity,        "Gravity",         -100.f, 100.f)
    REFLECT_HEADER   ("Color")
    REFLECT_COLOR4   (startColor,     "Start Color")
    REFLECT_COLOR4   (endColor,       "End Color")
    REFLECT_HEADER   ("Render")
    REFLECT_ENUM     (blendMode,      "Blend Mode",      kParticleBlendOptions)
    REFLECT_ENUM     (visualMode,     "Visual Mode",     kParticleVisualOptions)
    REFLECT_STRING_DROP(texturePath,  "Texture",         "ITEX_PATH")
    REFLECT_HEADER   ("Shape")
    REFLECT_ENUM     (shape,          "Shape",           kParticleShapeOptions)
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Sphere;
    })
        REFLECT_FLOAT(sphereRadius,       "Radius",            0.001f, 100.f)
        REFLECT_BOOL (sphereSpawnOnShell, "Spawn On Shell (surface only)")
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Cone;
    })
        REFLECT_ANGLE (coneHalfAngle, "Half Angle",      1.f, 89.f)
        REFLECT_FLOAT3(coneDirection, "Direction",      -1.f, 1.f)
        REFLECT_FLOAT (coneLength,    "Length",          0.f, 100.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Box;
    })
        REFLECT_FLOAT3(boxHalfExtents, "Half Extents",   0.001f, 100.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Circle;
    })
        REFLECT_FLOAT (circleRadius, "Radius",          0.001f, 100.f)
        REFLECT_FLOAT3(circleNormal, "Normal",         -1.f, 1.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Mesh;
    })
        REFLECT_UINT(meshSourceEntity, "Mesh Source Entity ID", 0u, 0xFFFFFFFEu)
        REFLECT_INFO("Pick an entity with MeshHandle or MeshLibRef")
    REFLECT_ENDIF()
REFLECT_END()

// ===========================================================================
// SkyboxComponent already declared above — keep minimal here.
// (No duplicate — searching for future maintainers.)
// ===========================================================================

// ===========================================================================
// AIComponent — enabled + tick interval reflected. Tree path is read-only
// info; per-tick trace lives in postDraw (needs BTAsset/BTNode lookups).
// ===========================================================================

REFLECT_BEGIN(AIComponent)
    REFLECT_BOOL (enabled,      "Enabled")
    REFLECT_FLOAT_FMT(tickInterval, "Tick Interval (s)", 0.f, 5.f, "%.3f", 0.005f)
    REFLECT_INFO_FN("Tree", [](const void* o, char* buf, std::size_t cap) {
        const auto* ai = static_cast<const AIComponent*>(o);
        std::snprintf(buf, cap, "%s",
            ai->treePath.empty() ? "(programmatic)" : ai->treePath.c_str());
    })
    REFLECT_INFO_FN("Time since last tick", [](const void* o, char* buf, std::size_t cap) {
        std::snprintf(buf, cap, "%.3fs",
            static_cast<const AIComponent*>(o)->timeSinceLastTick);
    })
REFLECT_END()

// ===========================================================================
// ECS::VolumeComponent — only the inner PostProcess::Volume field is
// reflectable (PostProcess::Volume itself); overrides stay in postDraw.
// ===========================================================================

constexpr Reflect::EnumOption kVolumeShapeOptions[] = {
    { (int)PostProcess::VolumeShape::Global, "Global" },
    { (int)PostProcess::VolumeShape::Box,    "Box"    },
    { (int)PostProcess::VolumeShape::Sphere, "Sphere" },
};

REFLECT_BEGIN(PostProcess::Volume)
    REFLECT_BOOL (enabled,  "Enabled")
    REFLECT_ENUM (shape,    "Shape", kVolumeShapeOptions)
    REFLECT_IF([](const void* o) {
        return static_cast<const PostProcess::Volume*>(o)->shape
            != PostProcess::VolumeShape::Global;
    })
        REFLECT_INFO ("Center follows entity GlobalTransform.")
        REFLECT_FLOAT3(extents, "Half-Extents (Sphere uses .x as radius)", 0.f, 1000.f)
        REFLECT_FLOAT (blendDistance, "Blend Distance", 0.f, 100.f)
    REFLECT_ENDIF()
    REFLECT_INT  (priority, "Priority", -1000, 1000)
REFLECT_END()

REFLECT_BEGIN(ECS::VolumeComponent)
    // Single nested PostProcess::Volume field — reuse its descriptor inline by
    // routing through a custom widget that delegates to DrawObject. Cheaper
    // than introducing a "nested struct" macro for one use site.
    REFLECT_CUSTOM(volume, "Volume", [](void* fp, const Reflect::FieldDescriptor&) {
        return Reflect::DrawObject(fp, Reflect::Describe<PostProcess::Volume>());
    })
REFLECT_END()

