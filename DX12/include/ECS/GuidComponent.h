#pragma once

// GuidComponent / PersistentTag / AttachmentRef — entity-identity & stable
// reference primitives. See DesignMd/entity_persistence_architecture.md.
//
//   GuidComponent     — entity carries a 128-bit stable identity. Serialized
//                       in scene. Required on any entity that something else
//                       refs across save/load.
//   PersistentTag     — marker for entities that should also be visited by
//                       the save-game writer (not the scene writer). Future
//                       save-game system filters by this tag.
//   AttachmentRef     — stable cross-entity reference. Carries the target's
//                       Guid on disk; resolves to a runtime Entity via
//                       GuidRegistry. Includes a cached Entity so steady-state
//                       access is one branch + one pointer load, not a hash
//                       probe.
//
// The "socket" half of AttachmentRef in the design doc (bone attachment) is
// not modelled here yet — the engine doesn't have a SocketAttachmentList /
// BoneAttachmentSystem to bind against. Reserved as a future extension.

#include "ECS/ECS.h"
#include "ECS/Guid.h"

#include <cstdint>

// GuidComponent — opt-in stable identity on an entity. Add it when something
// references this entity by anything other than its volatile Entity ID.
struct GuidComponent
{
    ECS::Guid guid = ECS::kInvalidGuid;
};

// PersistentTag — entity participates in save-game (in addition to scene
// save). Without it the entity is scene-scoped and recreated fresh next time
// the scene loads.
struct PersistentTag : ComponentBase {};

// AttachmentRef — a save-safe pointer to another entity. Stores the target's
// Guid for serialization; the runtime Entity / generation pair is cached so
// repeated access doesn't touch the registry hash table.
//
// Usage:
//   AttachmentRef target;
//   // ...serialized to/from disk via target.ownerGuid only...
//   if (Entity e = target.Resolve(world); e != NullEntity) {
//       // use e
//   }
//
// Resolve is cheap on the steady path: hits a `cachedOwner` after the first
// lookup, validates against the entity's generation, falls back to a registry
// lookup only when the cache is stale (entity died and slot was reused, or
// the world reloaded).
struct AttachmentRef
{
    // ---- Serialized -----------------------------------------------------
    ECS::Guid ownerGuid = ECS::kInvalidGuid;

    // ---- Runtime cache (not serialized) ---------------------------------
    // `cachedOwner` + `cachedGeneration` form an EntityHandle-style validity
    // check. A stale cache (entity destroyed, slot recycled, or world swap)
    // triggers a re-resolve through GuidRegistry.
    mutable Entity   cachedOwner      = NullEntity;
    mutable uint32_t cachedGeneration = 0u;

    bool HasGuid() const noexcept { return ownerGuid.IsValid(); }

    // Resolve the target entity for this frame. Returns NullEntity if the
    // GUID has no registered owner (target was destroyed, never loaded, or
    // never had a GuidComponent stamped).
    //
    // Defined out-of-line in GuidRegistry.cpp so we don't drag GuidRegistry's
    // header into every component-using TU.
    Entity Resolve(class World& world) const;

    // Bind to a specific entity by entity id. Pulls the Guid off the target's
    // GuidComponent. If the target has no GuidComponent the ref is cleared —
    // the caller is expected to stamp a Guid on the target first (or use
    // BindAndStamp). This is the editor "pick target" entry point.
    void Bind(class World& world, Entity target);

    // Bind + auto-stamp: ensures `target` has a GuidComponent (creating one
    // with a freshly-generated Guid if absent), then stores its Guid here.
    // Returns the Guid that ended up in `ownerGuid`. Use this from inspector
    // / drag-drop code where the user picks any entity regardless of whether
    // it was previously "ref-able".
    ECS::Guid BindAndStamp(class World& world, Entity target);

    // Drop the reference entirely.
    void Clear() noexcept
    {
        ownerGuid        = ECS::kInvalidGuid;
        cachedOwner      = NullEntity;
        cachedGeneration = 0u;
    }
};
