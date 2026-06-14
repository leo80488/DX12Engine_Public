#include "Scene/LuaSceneBindings.h"
#include "Scene/SceneManager.h"

#include <sol/sol.hpp>

#include <string>
#include <vector>

namespace Scene
{

void RegisterLuaSceneBindings(sol::state& lua, ::SceneManager& mgr)
{
    sol::table tbl = lua.create_table();

    // Scene.Load("name")  — faded transition (fade-out → loading → fade-in).
    // Accepts a registered scene name OR a raw .iscene path.
    tbl.set_function("Load",
        [&mgr](const std::string& name) { mgr.RequestLoad(name, /*fade=*/true); });

    // Scene.LoadInstant("name") — immediate cut, no fade (e.g. quick restarts).
    tbl.set_function("LoadInstant",
        [&mgr](const std::string& name) { mgr.RequestLoad(name, /*fade=*/false); });

    // Scene.Reload() — reload the current scene from disk.
    tbl.set_function("Reload",
        [&mgr]() { mgr.Reload(/*fade=*/true); });

    // Scene.Current() — the active scene's name.
    tbl.set_function("Current",
        [&mgr]() -> std::string { return mgr.CurrentScene(); });

    // Scene.List() — 1-based array table of registered scene names.
    tbl.set_function("List",
        [&mgr, &lua]() -> sol::table
        {
            sol::table out = lua.create_table();
            const std::vector<std::string> names = mgr.SceneNames();
            for (size_t i = 0; i < names.size(); ++i)
                out[i + 1] = names[i];
            return out;
        });

    lua["Scene"] = tbl;
}

} // namespace Scene
