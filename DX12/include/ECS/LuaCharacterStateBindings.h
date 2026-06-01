#pragma once

// Lua bindings for CharacterStateComponent. Exposes a small `Character.*`
// table that Logic scripts (per-entity) and BT actions (asset/ai/*.lua)
// share.
//
//   Character.AddState(entityId, name, clipPath, opts?)
//     opts = { loop=true, speed=1.0 } (optional)
//     Appends a StateEntry. Idempotent: same name overwrites the existing entry.
//
//   Character.SetState(entityId, name, blendDuration?)
//     Triggers a cross-fade toward `name`. blendDuration defaults to the
//     component's defaultBlendDuration. blendDuration == 0 snaps instantly.
//     Returns true on success, false when the entity has no CharacterState
//     component or the name isn't registered.
//
//   Character.GetState(entityId) -> string | nil
//     Returns the current state's name (the side that's playing in
//     `primary`), or nil when no state is set yet.
//
//   Character.IsBlending(entityId) -> bool

namespace sol { class state; }
class World;

void RegisterLuaCharacterStateBindings(sol::state& lua, World& world);
