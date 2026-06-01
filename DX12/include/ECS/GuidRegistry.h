#pragma once

// GuidRegistry — process-global Guid → Entity lookup used by AttachmentRef.
// See DesignMd/entity_persistence_architecture.md §6.3.
//
// Lifecycle:
//   - At App init: RegisterWithWorld(world) installs an EntityDestroyListener
//     so entities going away auto-unregister their GUID.
//   - After scene load: RebuildFromWorld(world) walks every GuidComponent
//     and re-populates the map. Cheap — one pass over the GuidComponent pool.
//   - On World::Clear: call Clear() to drop the whole map.
//
// Thread-safety: not thread-safe. All registry mutations run on the main
// thread (entity create/destroy, scene load) — matching the rest of the ECS.

#include "ECS/ECS.h"
#include "ECS/Guid.h"

#include <unordered_map>

class World;

namespace ECS
{

class GuidRegistry
{
public:
    // Process-wide singleton — App has only one World at a time today, and
    // when that changes we'll switch to a World-owned registry. The single
    // instance is enough until then.
    static GuidRegistry& Get();

    // Install destroy listener so entities with GuidComponent auto-unregister
    // when DestroyEntity runs. Idempotent — safe to call twice. Stores the
    // listener handle internally so a future Detach can clean up.
    void RegisterWithWorld(World& world);

    // Drop the destroy listener. Call before destroying the World it was
    // attached to.
    void DetachFromWorld(World& world);

    // Manual register/unregister — usually you only call these from the
    // GuidComponent serializer + RebuildFromWorld. Component code shouldn't
    // need these directly.
    void Register(Guid guid, Entity e);
    void Unregister(Guid guid);
    void UnregisterEntity(Entity e);  // for the destroy hook

    // Returns NullEntity when guid isn't registered.
    Entity Find(Guid guid) const noexcept;

    // Returns kInvalidGuid when entity isn't registered.
    Guid   ReverseFind(Entity e) const noexcept;

    // Wipe the entire map. Called from World::Clear path (App handles this).
    void Clear() noexcept;

    // Walk every GuidComponent in the world and (re)populate the map.
    // Drops the previous contents first. Use after a scene load.
    void RebuildFromWorld(World& world);

    size_t Size() const noexcept { return m_guidToEntity.size(); }

private:
    GuidRegistry()  = default;
    ~GuidRegistry() = default;
    GuidRegistry(const GuidRegistry&)            = delete;
    GuidRegistry& operator=(const GuidRegistry&) = delete;

    std::unordered_map<Guid, Entity> m_guidToEntity;
    std::unordered_map<Entity, Guid> m_entityToGuid;  // reverse map for unregister-by-entity

    // Destroy-listener handle for the world we hooked into. 0 = not hooked.
    World*   m_hookedWorld   = nullptr;
    uint32_t m_listenerHandle = 0u;
};

// Helper exposed to AttachmentRef::BindAndStamp — adds GuidComponent if
// absent (generating a fresh Guid) and returns the entity's Guid. Returns
// kInvalidGuid if `e` isn't alive.
Guid EnsureGuidOn(World& world, Entity e);

} // namespace ECS
