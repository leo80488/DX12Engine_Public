#pragma once

// Events emitted by the equipment Command API (Command::EquipToSocket /
// Command::UnequipItem). Kept separate from FollowEvents.h so subscribers
// that care about gameplay-level equipment don't pull in engine-internal
// follow bookkeeping.
//
// Both payloads carry EntityHandle (not raw Entity) so subscribers can
// safely Drop stale notifications in a later frame — e.g. if an unequip
// is queued and the item is destroyed before dispatch.

#include "ECS/ECS.h"

#include <cstdint>

// WeaponEquippedEvent — Command::EquipToSocket succeeded. socketIndex is the
// index into the character's SocketComponent::sockets at publish time; in
// unusual edge cases (socket removed mid-frame) it may be stale by the time
// a subscriber resolves it.
struct WeaponEquippedEvent
{
    EntityHandle character;
    EntityHandle item;
    uint32_t     socketIndex;
};

// WeaponUnequippedEvent — Command::UnequipItem succeeded, or EquipToSocket
// auto-detached a previously-equipped item before attaching it elsewhere.
// character may already be a stale handle if the unequip was triggered by
// the character's death.
struct WeaponUnequippedEvent
{
    EntityHandle character;
    EntityHandle item;
};
