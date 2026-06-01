#pragma once

// Lua bindings for PlayerComponent + CharacterControllerComponent. Registered
// by ScriptSystem::BindCustomTables at startup. Two tables are exposed:
//
//   Character.*  — operates on CharacterControllerComponent. Used to read
//                  ground state, override drive velocity, request jumps, or
//                  teleport the capsule.
//
//   Player.*     — operates on PlayerComponent. Used to adjust walk/run
//                  speeds, hook a camera entity, or read movement-feel
//                  parameters at runtime.
//
// Both expect the entity to already have the matching component; binding
// calls log a warning and return defaults / false when the component is
// missing rather than auto-attaching, so script-side typos don't silently
// spawn components on unrelated entities.

namespace sol { class state; }
class World;

void RegisterLuaPlayerBindings(sol::state& lua, World& world);
