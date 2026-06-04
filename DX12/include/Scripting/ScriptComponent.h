#pragma once

// ScriptComponent — attaches one or more Lua Logic scripts to an entity.
//
// Multiple scripts run independently (Unity-style component list): each slot
// gets its own per-entity Lua instance table, its own OnSpawn/OnUpdate/OnDestroy
// lifecycle, and its own editor-exposed variable overrides. Engine-driven
// events (OnEnter, OnAnimEvent) fire on EVERY attached script that defines the
// handler. The ScriptSystem owns the Lua VM; this component stores only the
// per-slot path + flags + var overrides. The sol::environment for each slot
// lives inside ScriptSystem (keyed by Entity + slot index) to avoid std::any
// copy issues with non-trivial types.

#include "Scripting/ScriptExposedVar.h"

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>

// One attached Lua Logic script and its per-entity authoring data.
struct ScriptInstance
{
    std::string scriptPath;    // e.g. "asset/scripts/rotate.lua"
    bool        enabled = true;

    // Per-entity overrides for editor-exposed script variables (declared via the
    // script's `exposed` table). Keyed by variable name; absent keys fall back to
    // the schema default. The editor writes these; ScriptSystem injects them onto
    // this slot's Lua instance table before OnSpawn so the script reads them as
    // `self.<name>`. Serialized inline on this slot's Script line.
    std::unordered_map<std::string, ScriptVarValue> vars;
};

struct ScriptComponent
{
    // Attached scripts, run in order. An empty vector is a valid (inert)
    // component — the editor "Add Component" path creates one before any
    // script slot is added.
    std::vector<ScriptInstance> scripts;
};
