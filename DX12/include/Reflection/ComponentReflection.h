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
#include "ECS/BillboardComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/DDGIComponents.h"
#include "ECS/TrailComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/PhysicsComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/ParticleComponent.h"
#include "ECS/TerrainComponent.h"
#include "ECS/SocketSystem.h"
#include "ECS/FollowComponents.h"
#include "ECS/VolumeComponent.h"
#include "AI/AIComponents.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Scripting/ScriptComponent.h"
#include "UI/UIComponents.h"
#include "UI/WorldSpaceUI.h"

#include <DirectXMath.h>
#include <unordered_map>

// ===========================================================================
// HierarchyComponents — pure data
// ===========================================================================

REFLECT_BEGIN(Visibility)
    REFLECT_BOOL(is_visible,        "Visible")
    REFLECT_BOOL(inherited_hidden,  "Inherited Hidden")
REFLECT_END()

REFLECT_BEGIN(RenderLayer)
    REFLECT_UINT(mask, "Layer Mask", 0u, 0xFFFFFFFFu)
REFLECT_END()

REFLECT_BEGIN(LocalAabb)
    REFLECT_FLOAT3(min, "Min", -1e6f, 1e6f)
    REFLECT_FLOAT3(max, "Max", -1e6f, 1e6f)
REFLECT_END()

// ===========================================================================
// CameraComponent — game-layer FPS camera
// ===========================================================================

REFLECT_BEGIN(CameraComponent)
    REFLECT_FLOAT3(position,         "Position",          -1e6f, 1e6f)
    REFLECT_SLIDER(yaw,              "Yaw (rad)",         -3.14159f, 3.14159f)
    REFLECT_SLIDER(pitch,            "Pitch (rad)",       -1.5f,     1.5f)
    REFLECT_ANGLE (fov,              "FOV",               10.f, 170.f)
    REFLECT_FLOAT_FMT(nearZ,         "Near Z",            0.001f,    10.f,   "%.3f", 0.001f)
    REFLECT_FLOAT_FMT(farZ,          "Far Z",             1.f,       5000.f, "%.0f", 1.f)
    REFLECT_FLOAT_FMT(mouseSensitivity, "Mouse Sensitivity", 0.0001f, 0.05f, "%.4f", 0.0001f)
    REFLECT_FLOAT_FMT(moveSpeed,     "Move Speed",        0.1f,      1000.f, "%.1f", 0.1f)
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
    REFLECT_FLOAT (ddgiDiffuseScale,      "DDGI Diffuse Scale",   0.f, 4.f)
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
REFLECT_END()

constexpr Reflect::EnumOption kColliderShapeOptions[] = {
    { (int)ColliderComponent::Shape::Box,     "Box"     },
    { (int)ColliderComponent::Shape::Sphere,  "Sphere"  },
    { (int)ColliderComponent::Shape::Capsule, "Capsule" },
};

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

REFLECT_BEGIN(ScriptComponent)
    REFLECT_BOOL       (enabled,    "Enabled")
    REFLECT_STRING_DROP(scriptPath, "Script Path", "ILUA_PATH")
    REFLECT_INFO       ("Drop a .lua file from the Resource Panel")
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

// Flat-ECS UI image — single textured quad without a Widget tree.
REFLECT_BEGIN(UI::UIImageComponent)
    REFLECT_BOOL  (visible, "Visible")
    REFLECT_FLOAT (sizeX,   "Size X (px)", 1.f, 4096.f)
    REFLECT_FLOAT (sizeY,   "Size Y (px)", 1.f, 4096.f)
    REFLECT_SLIDER(uv0X,    "UV0 X",       0.f, 1.f)
    REFLECT_SLIDER(uv0Y,    "UV0 Y",       0.f, 1.f)
    REFLECT_SLIDER(uv1X,    "UV1 X",       0.f, 1.f)
    REFLECT_SLIDER(uv1Y,    "UV1 Y",       0.f, 1.f)
    REFLECT_COLOR4(tint,    "Tint")
REFLECT_END()

// Flat-ECS UI text — single string. Inspector edits the `text` field live.
REFLECT_BEGIN(UI::UITextComponent)
    REFLECT_BOOL  (visible, "Visible")
    REFLECT_STRING(text,    "Text")
    REFLECT_COLOR4(color,   "Color")
    REFLECT_SLIDER(scale,   "Scale",       0.1f, 8.f)
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

