#pragma once

// Command API — Script → Engine intent-translation layer.
//
// ECS_Architecture_Design.md §"Command API：兩者之間的翻譯層":
//   Scripts never touch components directly. They call into this thin
//   facade; each function validates, mutates the relevant components, and
//   publishes the appropriate event. Side effects (logging, event dispatch,
//   future audio hooks) are centralized here so scripts stay ignorant of
//   engine internals.
//
// Lua binding counterparts live in ScriptSystem::RegisterBindings as the
// Engine.* table (Engine.EquipToSocket, Engine.UnequipItem, ...).
//
// Every function takes explicit World& to stay testable and to avoid
// hiding global state — callers are expected to route through the same
// World used by the rest of the frame pipeline.

#include "ECS/ECS.h"

namespace Command
{
    // Equip `item` to the socket named `socketName` on `character`.
    //
    // On success:
    //   - item gains FollowSocketComponent { target=character, socketIndex }
    //   - WeaponEquippedEvent is queued on the global EventBus
    //   - if item was already equipped elsewhere, WeaponUnequippedEvent is
    //     queued for the previous slot so subscribers see clean transitions
    //
    // Returns false (and logs a warning) if:
    //   - character or item is not alive
    //   - socketName is null/empty
    //   - character lacks SocketComponent
    //   - character's SocketComponent has no entry matching socketName
    bool EquipToSocket(World& world, Entity character, Entity item, const char* socketName);

    // Remove item's FollowSocketComponent and publish WeaponUnequippedEvent.
    // Returns false if the item had no FollowSocketComponent to begin with.
    bool UnequipItem(World& world, Entity item);

    // Generic entity-follow binding — thin AddComponent wrapper so Lua
    // doesn't have to know about FollowEntityComponent's internals.
    bool AttachToEntity(World& world, Entity follower, Entity target);

    // Remove follower's FollowEntityComponent.
    bool DetachFromEntity(World& world, Entity follower);
}
