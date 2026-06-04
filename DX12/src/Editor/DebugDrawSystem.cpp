#include "Editor/DebugDrawSystem.h"

#include "Graphics/Renderer.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/DebugIconPass.h"
#include "Nav/NavMeshSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Tools/CollisionMeshBaker.h"
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/FrameContext.h"

#include <DirectXMath.h>

// ===========================================================================
// Debug icon-kind registry — the single place that defines WHAT debug icons
// exist and HOW they map to entities. To add a new icon kind: drop a texture,
// add a DebugCategory bit (DebugCategory.h), and add ONE row to kIconKinds[].
// Everything else (GPU pass, texture loading, menu toggle) is generic.
// ---------------------------------------------------------------------------
namespace
{
    // Emit one icon at every entity that owns component T, at its world
    // position. Generic over the marker component (LightData, CameraComponent…).
    template <class T>
    void EmitIconsForPool(World& world, DebugIconPass& icons,
                          uint32_t texIdx, float halfSize)
    {
        auto* pool = world.GetPool<T>();
        if (!pool) return;
        const auto& ents = pool->Entities();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const Entity e = ents[i];
            if (!world.IsAlive(e)) continue;
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;
            icons.AddIcon({ gt->matrix._41, gt->matrix._42, gt->matrix._43 },
                          halfSize, texIdx);
        }
    }

    struct IconKind
    {
        DebugCategory category;
        const char*   name;          // Debug-menu label
        const char*   texturePath;   // .itex; resolved via Renderer::GetDebugIconBindless
        void (*emit)(World&, DebugIconPass&, uint32_t /*texIdx*/, float /*halfSize*/);
        float         halfSize;      // world-space half-extent of the quad
    };

    // ★ The registry. One row per debug icon kind. ★
    constexpr IconKind kIconKinds[] = {
        { DebugCategory::Lights,  "Light Icons",  "asset/Default_Texture/lightsymbol.itex",
          &EmitIconsForPool<LightData>,       0.25f },
        { DebugCategory::Cameras, "Camera Icons", "asset/Default_Texture/camerasymbol.itex",
          &EmitIconsForPool<CameraComponent>, 0.30f },
    };
    constexpr int kIconKindCount = static_cast<int>(sizeof(kIconKinds) / sizeof(kIconKinds[0]));
}

int DebugDrawSystem::IconKindCount() { return kIconKindCount; }

DebugDrawSystem::IconKindInfo DebugDrawSystem::GetIconKindInfo(int index)
{
    if (index < 0 || index >= kIconKindCount) return { "", DebugCategory::None };
    return { kIconKinds[index].name, kIconKinds[index].category };
}

// ---------------------------------------------------------------------------
// ApplyTo — push the central category state onto the renderer-owned buckets the
// BeginFrame-time wire gather reads. Must run BEFORE Renderer::BeginFrame.
// ---------------------------------------------------------------------------
void DebugDrawSystem::ApplyTo(Renderer& renderer, Nav::NavMeshSystem& nav) const
{
    if (DebugWirePass* dbg = renderer.GetDebugWirePass())
    {
        dbg->enabled              = masterEnabled;
        dbg->showAABBs            = Active(DebugCategory::AABB);
        dbg->showFrustum          = Active(DebugCategory::Frustum);
        dbg->showCapsules         = Active(DebugCategory::Capsules);
        dbg->showReflectionProbes = Active(DebugCategory::ReflectionProbes);
        dbg->showDDGIVolumes      = Active(DebugCategory::DDGIVolumes);
        dbg->showCollision        = Active(DebugCategory::Collision);
        dbg->collisionMaxDistance = collisionMaxDistance;
    }
    nav.debugDraw = Active(DebugCategory::NavMesh);
    // Light icons are no longer routed through the gameplay billboard path —
    // they are submitted to the dedicated DebugIconPass in Submit() below.
}

// ---------------------------------------------------------------------------
// Submit — emit the external wireframes (collision mesh, navmesh) for enabled
// categories. Must run AFTER Renderer::BeginFrame (ring slot live) and BEFORE
// Renderer::Render (which uploads + draws the wire buffer).
// ---------------------------------------------------------------------------
void DebugDrawSystem::Submit(Renderer& renderer, World& world, const FrameContext& ctx,
                             DX12Physics::PhysicsSystem& physics, Nav::NavMeshSystem& nav) const
{
    // ---- Debug icons → dedicated DebugIconPass (standalone gizmos, gated per
    //      category bit, independent of the wireframe master). Cleared every
    //      frame so toggling off / Game builds (Submit never called) leave the
    //      queue empty. Driven entirely by the kIconKinds[] registry. ---------
    if (DebugIconPass* icons = renderer.GetDebugIconPass())
    {
        icons->ClearIcons();
        for (const IconKind& kind : kIconKinds)
        {
            if (!DebugCatEnabled(categoryMask, kind.category)) continue;
            const uint32_t texIdx = renderer.GetDebugIconBindless(kind.texturePath);
            if (texIdx == 0xFFFFFFFFu) continue;   // texture missing / still loading
            kind.emit(world, *icons, texIdx, kind.halfSize);
        }
    }

    // ---- Wireframe overlays (gated by the master switch) -------------------
    if (!masterEnabled) return;
    DebugWirePass* dbg = renderer.GetDebugWirePass();
    if (!dbg) return;

    if (Active(DebugCategory::NavMesh))
        nav.EmitDebugLines(*dbg);

    if (Active(DebugCategory::Collision))
    {
        // `collisionMaxDistance <= 0` = unlimited; the Debug menu dials it down.
        const Entity camEnt = static_cast<Entity>(ctx.cameraEntity);
        if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(camEnt))
        {
            const DirectX::XMFLOAT3 camPos{
                gt->matrix._41, gt->matrix._42, gt->matrix._43 };
            Tools::CollisionMesh::EmitDebugWireframe(
                world, camPos, collisionMaxDistance, *dbg, physics);
        }
    }
}
