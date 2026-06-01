#pragma once

// ScriptComponent — attaches a Lua script to an entity.
// The ScriptSystem manages the Lua VM; this component stores only the path
// and runtime state flags. The sol::environment lives inside ScriptSystem
// (keyed by Entity) to avoid std::any copy issues with non-trivial types.

#include "Scripting/ScriptExposedVar.h"

#include <string>
#include <cstdint>
#include <unordered_map>

struct ScriptComponent
{
    std::string scriptPath;    // e.g. "asset/scripts/rotate.lua"
    bool        enabled = true;

    // Per-entity overrides for editor-exposed script variables (declared via the
    // script's `exposed` table). Keyed by variable name; absent keys fall back to
    // the schema default. The editor writes these; ScriptSystem injects them onto
    // the entity's Lua instance table before OnSpawn so the script reads them as
    // `self.<name>`. Serialized inline on the entity's Script line.
    std::unordered_map<std::string, ScriptVarValue> vars;
};
