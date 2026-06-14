#pragma once

// Lua bindings for the physics system. Currently exposes:
//   Physics.Raycast(fx, fy, fz, dx, dy, dz, maxDist)
//     -> (hit, hitX, hitY, hitZ, normalX, normalY, normalZ, entityId)
//
// Modelled after LuaNavBindings — registered once at startup, after the
// PhysicsSystem is initialised but before any user scripts run.

namespace sol { class state; }
class World;

namespace DX12Physics
{
    class PhysicsSystem;

    // `world` is needed by Physics.SpawnBall to create the ball entity +
    // attach the rigidbody/collider components.
    void RegisterLuaPhysicsBindings(sol::state& lua, PhysicsSystem& physics, World& world);
}
