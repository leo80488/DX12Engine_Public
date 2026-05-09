#pragma once

// LuaBTBindings — binds BTContext into the shared sol::state and creates
// the empty `Actions` / `Conditions` global tables that designers fill
// from .bt.lua / shared.lua files.
//
// Call once after ScriptSystem::Initialize, BEFORE any AI-related Lua
// scripts run.

namespace sol { class state; }

namespace AI
{
    void RegisterLuaBTBindings(sol::state& lua);
}
