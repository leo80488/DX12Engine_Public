#pragma once

// LuaNavBindings — exposes Nav.* on the engine's sol::state. Mirrors the
// AI::RegisterLuaBTBindings / UI::RegisterLuaUIBindings pattern; call once
// from App.cpp after ScriptSystem::Initialize.
//
// Lua surface (queries only — issuing intent is Intent.MoveTo / Intent.LookAt
// / Intent.Stop on Intent/LuaIntentBindings.cpp):
//   Nav.IsReady()                                      -> bool
//   Nav.FindPath(fromX,fromY,fromZ, toX,toY,toZ)       -> { {x,y,z}, ... }  (empty on failure)
//   Nav.Raycast (fromX,fromY,fromZ, toX,toY,toZ)       -> bool, hitX, hitY, hitZ
//   Nav.Project (worldX,worldY,worldZ)                  -> ok, x, y, z

namespace sol { class state; }
class World;   // global ::World

namespace Nav
{
    class NavMeshSystem;

    void RegisterLuaNavBindings(sol::state& lua, NavMeshSystem& nav, ::World& world);
}
