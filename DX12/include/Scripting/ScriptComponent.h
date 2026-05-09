#pragma once

// ScriptComponent — attaches a Lua script to an entity.
// The ScriptSystem manages the Lua VM; this component stores only the path
// and runtime state flags. The sol::environment lives inside ScriptSystem
// (keyed by Entity) to avoid std::any copy issues with non-trivial types.

#include <string>
#include <cstdint>

struct ScriptComponent
{
    std::string scriptPath;    // e.g. "asset/scripts/rotate.lua"
    bool        enabled = true;
};
