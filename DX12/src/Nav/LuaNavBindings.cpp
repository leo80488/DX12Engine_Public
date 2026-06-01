#include "Nav/LuaNavBindings.h"
#include "Nav/NavMeshSystem.h"

#include "ECS/ECS.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include <DirectXMath.h>
#include <tuple>

namespace Nav
{

void RegisterLuaNavBindings(sol::state& lua, NavMeshSystem& nav, ::World& /*world*/)
{
    sol::table tbl = lua.create_table();

    tbl.set_function("IsReady", [&nav]() -> bool { return nav.IsReady(); });

    // Returns a Lua array of {x, y, z} tables. Empty array if FindPath fails
    // or no path exists.
    tbl.set_function("FindPath",
        [&nav, &lua](float fx, float fy, float fz, float tx, float ty, float tz) -> sol::table
        {
            sol::table out = lua.create_table();
            if (!nav.IsReady()) return out;
            PathResult pr = nav.FindPath({ fx, fy, fz }, { tx, ty, tz });
            if (!pr.valid) return out;
            int idx = 1;
            for (const auto& p : pr.waypoints)
            {
                sol::table wp = lua.create_table();
                wp["x"] = p.x; wp["y"] = p.y; wp["z"] = p.z;
                out[idx++] = wp;
            }
            return out;
        });

    // Returns multiple values: clear?, hitX, hitY, hitZ
    tbl.set_function("Raycast",
        [&nav](float fx, float fy, float fz, float tx, float ty, float tz)
            -> std::tuple<bool, float, float, float>
        {
            if (!nav.IsReady()) return { false, tx, ty, tz };
            DirectX::XMFLOAT3 hit{ tx, ty, tz };
            const bool clear = nav.Raycast({ fx, fy, fz }, { tx, ty, tz }, &hit);
            return { clear, hit.x, hit.y, hit.z };
        });

    tbl.set_function("Project",
        [&nav](float x, float y, float z)
            -> std::tuple<bool, float, float, float>
        {
            DirectX::XMFLOAT3 onMesh{};
            const bool ok = nav.ProjectToMesh({ x, y, z }, onMesh);
            return { ok, onMesh.x, onMesh.y, onMesh.z };
        });

    // Intent.MoveTo / Intent.Stop / Intent.LookAt / Intent.ClearLookAt now
    // live on Intent/LuaIntentBindings.cpp — they used to be Nav.SetAgentTarget
    // etc., but tactical movement intent is no longer a Nav concern.

    lua["Nav"] = tbl;
    LOG_INFO("Nav: registered Lua bindings");
}

} // namespace Nav
