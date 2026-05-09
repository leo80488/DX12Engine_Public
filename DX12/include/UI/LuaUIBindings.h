#pragma once

// LuaUIBindings — exposes the UI module to Lua via sol3.
//
// API surface (Lua side):
//   local hud = ui.Canvas{ size = {1920, 1080} }
//   ui.Button{ parent = hud, anchor = "center", size = {200, 60},
//              text = "Start", onClick = function() Game.Start() end }
//   ui.Text{ parent = hud, anchor = "top-left", offset = {20, 20},
//            text = "FPS: 60", color = 0xFFFFFFFF }
//   local hp = ui.ProgressBar{ parent = hud, anchor = "top-left",
//                              offset = {20, 80}, size = {300, 24} }
//   hp:SetValue(0.5)
//   ui.MountRoot("HUD", hud, 0)  -- creates an Entity with UIRootComponent
//
// Widget objects exposed to Lua are WidgetHandle userdata (not raw ptrs);
// generation-checked Get() means a destroyed widget yields nil safely.
//
// Call once after ScriptSystem::Initialize and BEFORE any UI scripts run.

namespace sol { class state; }
class World;

namespace UI
{
    void RegisterLuaUIBindings(sol::state& lua, World& world);
}
