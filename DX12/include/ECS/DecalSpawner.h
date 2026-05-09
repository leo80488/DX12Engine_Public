#pragma once

// DecalSpawner — gameplay-facing helper for creating dynamic decals.
//
// Two modes:
//   1. Pool mode   : InitPool(world, N) pre-allocates N entities; Spawn* calls
//                    round-robin through them, recycling the oldest when full.
//                    Pool slots carry destroyEntityOnExpire=false so the
//                    DecalLifetimeSystem only removes the DecalComponent at
//                    expiry — the entity + transform stay alive for the
//                    spawner to reuse. Best for FPS-style hit effects that
//                    spawn many times per second.
//   2. Fresh mode  : Without an Init'd pool, every Spawn* allocates a new
//                    entity (destroyEntityOnExpire=true by default). Simpler
//                    mental model for low-frequency spawns (set pieces, hero
//                    effects) where entity churn is negligible.
//
// Typical game code (pool mode):
//   renderer.GetDecalSpawner().InitPool(world, 256);  // scene init
//   ...
//   auto bloodMat = renderer.GetDecalMaterialLibrary().Find("blood_a");
//   renderer.GetDecalSpawner().SpawnAtSurface(world, bloodMat,
//                                             hit.pos, hit.normal,
//                                             {0.25f, 0.25f}, 0.1f,
//                                             /*lifetime*/ 10.0f);

#include "ECS/ECS.h"
#include <DirectXMath.h>
#include <memory>
#include <cstdint>

namespace Resource { class DecalMaterialAsset; }

class DecalSpawner
{
public:
    DecalSpawner() = default;

    // Allocate @p capacity entities up-front. Each slot gets a LocalTransform
    // + GlobalTransform so future Spawn calls only need to write matrices.
    // Calling InitPool a second time dumps the old pool list without
    // destroying entities — the caller is expected to call World::Clear()
    // first if reinitialising with a different world.
    void InitPool(World& world, uint32_t capacity);

    // Called by Renderer::OnWorldClear. Drops the slot list (entities already
    // gone via World::Clear()).
    void OnWorldClear();

    // Spawn with an explicit TRS. Returns the entity owning the new
    // DecalComponent, or NullEntity if @p material is null. Both
    // LocalTransform and GlobalTransform are written so the decal is visible
    // the same frame (TransformSystem will recompute GlobalTransform next
    // frame from LocalTransform — produces the same result for leaf entities).
    Entity Spawn(World&                                          world,
                 std::shared_ptr<Resource::DecalMaterialAsset>   material,
                 const DirectX::XMFLOAT3&                        position,
                 const DirectX::XMFLOAT4&                        rotationQuat,
                 const DirectX::XMFLOAT3&                        scale,
                 float                                            lifetime,
                 float                                            fadeOutDuration = 0.5f);

    // High-level helper: orient a decal to stick onto a surface. Builds a
    // rotation that maps local +Z onto -surfaceNormal — the direction the
    // Apply CS's angleFade term prefers.
    //   @p size      — X/Y  decal width/height in world units.
    //   @p depth     — thickness along the projection axis (clipping box Z).
    //   @p rollZ     — optional rotation around the projection axis for
    //                  visual variation between instances of the same
    //                  material (e.g. non-square bullet-hole texture).
    Entity SpawnAtSurface(World&                                         world,
                          std::shared_ptr<Resource::DecalMaterialAsset>  material,
                          const DirectX::XMFLOAT3&                       hitPos,
                          const DirectX::XMFLOAT3&                       surfaceNormal,
                          const DirectX::XMFLOAT2&                       size,
                          float                                           depth,
                          float                                           lifetime,
                          float                                           fadeOutDuration = 0.5f,
                          float                                           rollZ           = 0.0f);

    // Stats for profiler / debug UI.
    uint32_t GetPoolCapacity() const { return static_cast<uint32_t>(m_slots.size()); }
    uint32_t GetActiveCount(const World& world) const;

private:
    // Produce an entity suitable for a fresh DecalComponent. In pool mode
    // returns the next slot (round-robin). In fresh mode allocates via
    // World::CreateEntity + seeds Local/Global transforms.
    Entity AcquireSlot(World& world);

    std::vector<Entity> m_slots;   // empty => pool disabled, fresh-mode spawn
    uint32_t            m_cursor = 0;
};
