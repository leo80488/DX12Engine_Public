#pragma once

// AILODSystem — adjusts AIComponent::tickInterval each frame based on
// distance from the camera. Cheaper to make distant AI tick at 1 Hz than
// to write per-AI custom budgeting.
//
// Tiers (matches BT_AI_System_Architecture.md §5.3):
//   Boss (override) — caller marks an entity as Tier-1 via SetBoss()
//   Combat          — within combat radius
//   Distant         — within world radius
//   Off-screen      — beyond world radius
//
// "In view" cull uses camera position only (no frustum math here) — when
// the engine grows a frustum-cull system, swap that in.

#include "ECS/ECS.h"
#include <unordered_set>

namespace AI
{
    class AILODSystem
    {
    public:
        struct Tiers
        {
            float bossInterval     = 1.f / 30.f;
            float combatInterval   = 1.f / 10.f;
            float distantInterval  = 1.f;
            float offscreenInterval = 5.f;

            float combatRadius   = 25.f;
            float distantRadius  = 80.f;
        };

        Tiers tiers{};

        // Mark an entity as boss / hero. Always uses bossInterval as long
        // as the entity is alive. Caller is responsible for clearing the
        // tag on death.
        void SetBoss(Entity e)   { m_bosses.insert(e); }
        void ClearBoss(Entity e) { m_bosses.erase(e); }

        // Sets cameraEntity used for distance queries. Pass NullEntity to
        // disable distance LOD (every AI then uses combatInterval).
        void SetCameraEntity(Entity e) { m_camera = e; }

        // Walk every AIComponent and rewrite tickInterval.
        void Update(World& world);

    private:
        Entity                    m_camera = NullEntity;
        std::unordered_set<Entity> m_bosses;
    };
}
