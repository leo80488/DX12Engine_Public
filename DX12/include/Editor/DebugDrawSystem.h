#pragma once

// DebugDrawSystem — the single, editor-driven submission point for ALL editor
// debug visuals (collision / AABB / frustum / capsule / probe / DDGI wireframes,
// navmesh, and light billboard icons).
//
// Why this exists (see editor_debug_draw_system.md): debug submission used to be
// scattered — an inline block in App's render lambda, ECS-pool walks inside the
// Renderer, plus a fistful of toggle bools spread across DebugWirePass,
// NavMeshSystem and Renderer. This class centralizes the toggle state into ONE
// category bitmask and routes every submission through one place, so the editor
// owns "what debug to show" and gameplay/rendering systems stay unaware of it.
//
// Lifecycle each editor frame (App's render lambda, all calls WITH_EDITOR-gated
// so debug draw is stripped from Game/ShaderLab builds):
//   1. ApplyTo(renderer, nav)  — BEFORE Renderer::BeginFrame: push the category
//      mask down onto the renderer-owned buckets (DebugWirePass flags, navmesh
//      flag, light-icon visibility) that the BeginFrame-time gather reads.
//   2. Renderer::BeginFrame()  — clears the wire ring buffer + gathers the
//      renderer-internal wireframes (AABB/capsule/frustum/probe/DDGI) for the
//      enabled categories.
//   3. Submit(renderer, world, ctx, physics, nav) — AFTER BeginFrame (ring slot
//      live) and BEFORE Render: emit the external wireframes (collision mesh,
//      navmesh) for the enabled categories.
//
// The class itself is plain driver code (lives in EngineCore so every exe links
// it); the editor-only gating is at the App call sites.

#include "Editor/DebugCategory.h"
#include <cstdint>

class Renderer;
class World;
struct FrameContext;
namespace Nav        { class NavMeshSystem; }
namespace DX12Physics { class PhysicsSystem; }

class DebugDrawSystem
{
public:
    // ---- Central toggle state (replaces the old scattered bools) -----------
    // Master switch for the wireframe overlays. Light icons are independent of
    // it (they are a standalone gizmo), gated only by the Lights category bit.
    bool     masterEnabled       = false;
    // Categories enabled. Lights ON by default so icons show in a fresh editor;
    // everything else OFF (matches the old per-flag defaults).
    uint32_t categoryMask        = static_cast<uint32_t>(DebugCategory::Lights);
    // 0 = unlimited collision-wireframe range; the Debug menu dials it down.
    float    collisionMaxDistance = 0.0f;

    // Is a WIREFRAME category active this frame? Wireframe overlays require the
    // master switch. (Icon categories like Lights/Cameras are standalone gizmos
    // gated directly by their bit — see the icon-kind registry below.)
    bool Active(DebugCategory c) const
    {
        return masterEnabled && DebugCatEnabled(categoryMask, c);
    }

    // ---- Debug icon kinds (data-driven registry) ---------------------------
    // Adding a NEW debug icon (e.g. camera / audio / spawn point) takes three
    // small steps and NO new GPU/loading code:
    //   1. drop a texture, e.g. asset/Default_Texture/mysymbol.itex
    //   2. add one DebugCategory bit in DebugCategory.h
    //   3. add one entry to kIconKinds[] in DebugDrawSystem.cpp:
    //        { DebugCategory::MyThing, "My Icons", "asset/.../mysymbol.itex",
    //          &EmitIconsForPool<MyComponent>, 0.25f }
    // Both the Debug-menu toggle and the rendering then pick it up via these
    // accessors. The icon's bindless texture is resolved generically through
    // Renderer::GetDebugIconBindless().
    struct IconKindInfo { const char* name; DebugCategory category; };
    static int          IconKindCount();
    static IconKindInfo GetIconKindInfo(int index);

    // Push the central state onto the renderer-owned buckets. Call BEFORE
    // Renderer::BeginFrame (whose wire-gather reads these flags).
    void ApplyTo(Renderer& renderer, Nav::NavMeshSystem& nav) const;

    // Emit the external wireframes (collision mesh + navmesh) for the enabled
    // categories. Call AFTER Renderer::BeginFrame and BEFORE Renderer::Render.
    void Submit(Renderer& renderer, World& world, const FrameContext& ctx,
                DX12Physics::PhysicsSystem& physics, Nav::NavMeshSystem& nav) const;
};
