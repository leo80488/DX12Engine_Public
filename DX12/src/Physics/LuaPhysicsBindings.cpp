#include "Physics/LuaPhysicsBindings.h"
#include "Physics/PhysicsSystem.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform
#include "ECS/PhysicsComponents.h"     // RigidBodyComponent / ColliderComponent
#include "Scene/MeshSpawner.h"         // MeshSpawner::Spawn
#include "Resource/ProceduralMesh.h"   // PrimitiveMeshType
#include "System/Log.h"

#include <sol/sol.hpp>

#include <tuple>
#include <utility>

namespace DX12Physics
{

void RegisterLuaPhysicsBindings(sol::state& lua, PhysicsSystem& physics, World& world)
{
    sol::table tbl = lua.create_table();

    // Raycast — returns:
    //   hit       : bool
    //   hitX/Y/Z  : world-space hit point (only meaningful when hit == true)
    //   nX/nY/nZ  : surface normal at the hit
    //   entityId  : owning entity (NullEntity sentinel when the body had no
    //               associated entity — e.g. editor-only bodies)
    //
    // Direction does not need to be normalised; caller passes the world-space
    // heading and the maxDist scalar gates how far the ray travels.
    tbl.set_function("Raycast",
        [&physics](float fx, float fy, float fz,
                   float dx, float dy, float dz,
                   float maxDist)
            -> std::tuple<bool, float, float, float,
                          float, float, float, uint32_t>
        {
            const RayHit r = physics.CastRayClosest({ fx, fy, fz },
                                                    { dx, dy, dz },
                                                    maxDist);
            return { r.hit,
                     r.point.x,  r.point.y,  r.point.z,
                     r.normal.x, r.normal.y, r.normal.z,
                     static_cast<uint32_t>(r.entity) };
        });

    // Physics.SpawnBall(x,y,z, radius, vx,vy,vz) -> entityId
    //   A VISIBLE dynamic sphere with a sphere collider, launched along
    //   (vx,vy,vz). The Jolt body is created lazily on the next PhysicsSystem
    //   tick; the launch velocity rides along on RigidBodyComponent and is
    //   applied at that creation point (see PhysicsSystem Phase 1).
    tbl.set_function("SpawnBall",
        [&world](float x, float y, float z,
                 float radius,
                 float vx, float vy, float vz) -> uint32_t
        {
            if (radius <= 0.f) radius = 0.25f;

            // Visual: the pre-uploaded GPU primitive sphere (base radius 0.5)
            // + default material + all render components (MeshSpawner).
            Entity e = MeshSpawner::Spawn(
                static_cast<int>(PrimitiveMeshType::Sphere), world);
            if (e == NullEntity) return static_cast<uint32_t>(NullEntity);

            // The primitive sphere has base radius 0.5, and MakeShape now scales
            // the collider by the entity world scale too — so a single scale of
            // 2*radius drives BOTH the visible mesh (0.5 * 2r = r) and the
            // physics sphere (0.5 * 2r = r) to the requested radius.
            if (auto* lt = world.GetComponent<LocalTransform>(e))
            {
                lt->translation = { x, y, z };
                lt->scale       = { radius * 2.f, radius * 2.f, radius * 2.f };
            }

            // Sphere collider at the primitive's base radius (0.5); the entity
            // scale above sizes it to `radius`.
            ColliderComponent col;
            col.shape  = ColliderComponent::Shape::Sphere;
            col.radius = 0.5f;
            world.AddComponent<ColliderComponent>(e, std::move(col));

            // Dynamic body carrying the one-shot launch velocity.
            RigidBodyComponent rb;
            rb.motion             = RigidBodyComponent::Motion::Dynamic;
            rb.mass               = 1.0f;
            rb.initialVelocity    = { vx, vy, vz };
            rb.hasInitialVelocity = true;
            world.AddComponent<RigidBodyComponent>(e, std::move(rb));

            return static_cast<uint32_t>(e);
        });

    lua["Physics"] = tbl;
    LOG_INFO("Physics: registered Lua bindings");
}

} // namespace DX12Physics
