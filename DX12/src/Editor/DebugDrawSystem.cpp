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
#include "ECS/PostProcessVolumeComponent.h"

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

    // ---- Post-process volume gizmos ---------------------------------------
    using namespace DirectX;

    // Draw an OBB (center+rotation+half-extents) as 12 edges.
    void DrawOBB(DebugWirePass& dbg, FXMVECTOR center, FXMVECTOR rotQ,
                 const XMFLOAT3& half, uint32_t color)
    {
        XMFLOAT3 c[8];
        for (int i = 0; i < 8; ++i)
        {
            const float sx = (i & 1) ? 1.f : -1.f;
            const float sy = (i & 2) ? 1.f : -1.f;
            const float sz = (i & 4) ? 1.f : -1.f;
            XMVECTOR local = XMVectorSet(sx * half.x, sy * half.y, sz * half.z, 0.f);
            XMVECTOR world = XMVectorAdd(center, XMVector3Rotate(local, rotQ));
            XMStoreFloat3(&c[i], world);
        }
        static const int e[12][2] = {
            {0,1},{2,3},{4,5},{6,7},  // x edges
            {0,2},{1,3},{4,6},{5,7},  // y edges
            {0,4},{1,5},{2,6},{3,7},  // z edges
        };
        for (auto& edge : e) dbg.AddLine(c[edge[0]], c[edge[1]], color);
    }

    // Draw a sphere as three great-circle rings.
    void DrawSphere(DebugWirePass& dbg, const XMFLOAT3& center, float radius, uint32_t color)
    {
        constexpr int kSeg = 24;
        for (int axis = 0; axis < 3; ++axis)
        {
            XMFLOAT3 prev{};
            for (int s = 0; s <= kSeg; ++s)
            {
                const float a = (float(s) / kSeg) * XM_2PI;
                const float u = std::cos(a) * radius;
                const float v = std::sin(a) * radius;
                XMFLOAT3 p = center;
                if (axis == 0)      { p.x += u; p.y += v; }
                else if (axis == 1) { p.y += u; p.z += v; }
                else                { p.x += u; p.z += v; }
                if (s > 0) dbg.AddLine(prev, p, color);
                prev = p;
            }
        }
    }

    void EmitVolumeGizmos(World& world, DebugWirePass& dbg)
    {
        auto* pool = world.GetPool<ECS::PostProcessVolumeComponent>();
        if (!pool) return;
        constexpr uint32_t kBounds = 0xFF66CCFFu;  // light blue
        constexpr uint32_t kShell  = 0x8033AACCu;  // dimmer, translucent-ish

        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const ECS::PostProcessVolumeComponent& v = data[i];
            if (v.isGlobal) continue;  // unbounded — nothing spatial to draw
            const Entity e = ents[i];
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;

            const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
            XMVECTOR scale, rotQ, trans;
            if (!XMMatrixDecompose(&scale, &rotQ, &trans, M)) continue;
            XMFLOAT3 half; XMStoreFloat3(&half, scale);

            if (v.shape == ECS::PPVolumeShape::Sphere)
            {
                XMFLOAT3 c; XMStoreFloat3(&c, trans);
                const float r = half.x;
                DrawSphere(dbg, c, r, kBounds);
                if (v.blendDistance > 0.f) DrawSphere(dbg, c, r + v.blendDistance, kShell);
            }
            else
            {
                DrawOBB(dbg, trans, rotQ, half, kBounds);
                if (v.blendDistance > 0.f)
                {
                    const XMFLOAT3 outer{ half.x + v.blendDistance,
                                          half.y + v.blendDistance,
                                          half.z + v.blendDistance };
                    DrawOBB(dbg, trans, rotQ, outer, kShell);
                }
            }
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

    if (Active(DebugCategory::PostProcessVolumes))
        EmitVolumeGizmos(world, *dbg);

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
