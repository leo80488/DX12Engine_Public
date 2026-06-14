#pragma once

// Lua bindings for the data-driven scene system. Exposes a global `Scene`
// table that forwards to the C++ SceneManager:
//   Scene.Load(name)        -> queue a faded transition to a registered scene
//                              name (or a raw .iscene path)
//   Scene.LoadInstant(name) -> same, but an instant cut (no fade)
//   Scene.Reload()          -> reload the current scene
//   Scene.Current()         -> current scene name (string)
//   Scene.List()            -> array table of registered scene names
//
// Registered once at startup, after ScriptSystem::Initialize() (which creates
// the Lua VM) but before any user scripts run. Modelled after the other
// Register*Bindings entry points.

namespace sol { class state; }
class SceneManager;

namespace Scene
{
    void RegisterLuaSceneBindings(sol::state& lua, ::SceneManager& mgr);
}
