#include "Physics/LuaPhysicsBindings.h"
#include "Physics/PhysicsSystem.h"

#include "System/Log.h"

#include <sol/sol.hpp>

#include <tuple>

namespace DX12Physics
{

void RegisterLuaPhysicsBindings(sol::state& lua, PhysicsSystem& physics)
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

    lua["Physics"] = tbl;
    LOG_INFO("Physics: registered Lua bindings");
}

} // namespace DX12Physics
