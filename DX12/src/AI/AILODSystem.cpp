#include "AI/AILODSystem.h"
#include "AI/AIComponents.h"
#include "ECS/Components.h"            // CameraComponent
#include "ECS/HierarchyComponents.h"   // GlobalTransform

#include <DirectXMath.h>

namespace AI
{
    static float DistanceSq(const DirectX::XMFLOAT4X4& m,
                            const DirectX::XMFLOAT3&   p)
    {
        const float dx = m._41 - p.x;
        const float dy = m._42 - p.y;
        const float dz = m._43 - p.z;
        return dx*dx + dy*dy + dz*dz;
    }

    void AILODSystem::Update(World& world)
    {
        // Camera position — fall back to (0,0,0) if no camera or no
        // CameraComponent. Tier selection still works because every AI
        // gets compared against the same origin.
        DirectX::XMFLOAT3 camPos{ 0.f, 0.f, 0.f };
        bool haveCamera = false;
        if (m_camera != NullEntity)
        {
            if (auto* cam = world.GetComponent<CameraComponent>(m_camera))
            {
                camPos     = cam->position;
                haveCamera = true;
            }
        }

        const float combatRSq  = tiers.combatRadius  * tiers.combatRadius;
        const float distantRSq = tiers.distantRadius * tiers.distantRadius;

        auto* pool = world.GetPool<AIComponent>();
        if (!pool) return;
        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        const size_t n = ents.size();
        for (size_t i = 0; i < n; ++i)
        {
            AIComponent& ai = data[i];
            const Entity e  = ents[i];

            if (m_bosses.count(e))
            {
                ai.tickInterval = tiers.bossInterval;
                continue;
            }
            if (!haveCamera)
            {
                ai.tickInterval = tiers.combatInterval;
                continue;
            }

            const auto* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt)
            {
                ai.tickInterval = tiers.combatInterval;
                continue;
            }
            const float d2 = DistanceSq(gt->matrix, camPos);
            if      (d2 <= combatRSq)  ai.tickInterval = tiers.combatInterval;
            else if (d2 <= distantRSq) ai.tickInterval = tiers.distantInterval;
            else                       ai.tickInterval = tiers.offscreenInterval;
        }
    }
}
