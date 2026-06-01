#pragma once

// CommandBuffer — defers structural ECS mutations to the phase boundary.
//
// Iterating a ComponentPool while another system in the same Phase calls
// AddComponent/RemoveComponent/DestroyEntity on the same pool will reshape
// the dense vectors mid-iteration. The Scheduler hands every phase a
// CommandBuffer; systems queue mutations here instead of touching the
// World directly, and the Scheduler flushes once Phase::Update returns
// for every system.
//
// Note: CreateEntity is intentionally NOT exposed. Allocating an ID without
// touching the World's free-list would require a complex "future entity"
// handle that resolves at flush time, and the only current call sites
// (script Spawn, projectile fire, BT spawn) can drop into GameplayPreLogic
// where they're already serial against gameplay iteration. If a real need
// arises later, add a deferred Create that returns a stable handle.

#include "ECS.h"  // Entity, World

#include <functional>
#include <utility>
#include <vector>

class CommandBuffer
{
public:
    // Queue an entity for destruction. Safe if e is already dead at flush
    // time (skipped silently) — covers the case where two systems both
    // queue Destroy(e) in the same phase.
    void DestroyEntity(Entity e)
    {
        m_commands.emplace_back([e](World& w) {
            if (w.IsAlive(e)) w.DestroyEntity(e);
        });
    }

    // Queue an AddComponent. Value is moved into the deferred closure so
    // the system's local doesn't have to outlive the phase.
    template <typename T>
    void AddComponent(Entity e, T value)
    {
        m_commands.emplace_back(
            [e, v = std::move(value)](World& w) mutable {
                if (w.IsAlive(e)) w.AddComponent<T>(e, std::move(v));
            });
    }

    template <typename T>
    void RemoveComponent(Entity e)
    {
        m_commands.emplace_back([e](World& w) {
            if (w.IsAlive(e)) w.RemoveComponent<T>(e);
        });
    }

    // Generic escape hatch for ops the typed wrappers don't cover (e.g.
    // rename, re-parent, attach socket, fire an event). Caller is
    // responsible for the lambda being safe to invoke against the world
    // at flush time.
    void Defer(std::function<void(World&)> fn)
    {
        m_commands.emplace_back(std::move(fn));
    }

    // Apply every queued command in FIFO order. Commands enqueued from
    // inside a flush callback land in the NEXT flush — we swap to a local
    // list first to make that semantics explicit and re-entrancy-safe.
    void Flush(World& world)
    {
        if (m_commands.empty()) return;
        std::vector<std::function<void(World&)>> local;
        local.swap(m_commands);
        for (auto& cmd : local) cmd(world);
    }

    bool   Empty() const { return m_commands.empty(); }
    size_t Size()  const { return m_commands.size(); }

private:
    std::vector<std::function<void(World&)>> m_commands;
};
