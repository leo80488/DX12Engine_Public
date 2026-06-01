#pragma once

// ISystem — base class for everything the Scheduler ticks.
//
// One ISystem belongs to exactly one TickPhase. The Scheduler calls Update
// once per frame (or once per fixed step, for systems in fixed-timestep
// phases). Systems declare the component types they Read / Write via
// DeclareAccess so the Scheduler can batch them in parallel (S3+);
// systems that omit the declaration are treated as conservative
// "read+write everything" and serialise.

#include "TickPhase.h"

#include <typeindex>
#include <unordered_set>

class World;
struct FrameContext;

// Records the component-type access set for one system. The Scheduler
// uses two systems' builders to decide whether they can run concurrently:
// they can iff (neither is opaque) AND no W-W or R-W overlap exists on
// any component or exclusive resource.
//
// Default state is OPAQUE — i.e. "I read/write arbitrary global state,
// don't co-batch me with anyone". Systems that want to opt INTO parallel
// batching MUST override ISystem::DeclareAccess and call Read/Write
// (or do nothing, if they truly touch no state).
class SystemAccessBuilder
{
public:
    template <typename T>
    SystemAccessBuilder& Read()
    {
        m_reads.insert(std::type_index(typeid(T)));
        return *this;
    }

    template <typename T>
    SystemAccessBuilder& Write()
    {
        m_writes.insert(std::type_index(typeid(T)));
        return *this;
    }

    // For non-component shared state — e.g. the Jolt PhysicsSystem singleton,
    // an AudioEngine voice pool, the bindless descriptor heap. Same ID twice
    // → cannot run concurrently. Caller picks IDs out of a private namespace.
    SystemAccessBuilder& ExclusiveResource(uint64_t resourceId)
    {
        m_exclusives.insert(resourceId);
        return *this;
    }

    // Marks this system as touching arbitrary global state — the Scheduler
    // will serialise it against every other system in the same phase. This
    // is the default (see ISystem::DeclareAccess) so any system that hasn't
    // been audited stays safe.
    SystemAccessBuilder& OpaqueAccess() { m_opaque = true; return *this; }

    const std::unordered_set<std::type_index>& Reads()      const { return m_reads; }
    const std::unordered_set<std::type_index>& Writes()     const { return m_writes; }
    const std::unordered_set<uint64_t>&        Exclusives() const { return m_exclusives; }
    bool                                       IsOpaque()   const { return m_opaque; }

private:
    std::unordered_set<std::type_index> m_reads;
    std::unordered_set<std::type_index> m_writes;
    std::unordered_set<uint64_t>        m_exclusives;
    bool                                m_opaque = false;
};

class ISystem
{
public:
    virtual ~ISystem() = default;

    // Phase this system lives in. SystemInPhase<P> implements this for you.
    virtual TickPhase   GetPhase() const = 0;

    // Stable identifier for profilers / ImGui phase visualisation.
    virtual const char* GetName()  const = 0;

    // Called once at startup, in registration order. Use for caching
    // World references, subscribing to event bus, etc. Do NOT mutate
    // game state here — Scheduler hasn't entered the tick loop yet.
    virtual void OnRegister(World& /*world*/) {}

    // Called once at shutdown, in REVERSE registration order.
    virtual void OnUnregister(World& /*world*/) {}

    // Per-frame work. For systems in fixed-timestep phases this may be
    // called 0 or N times per render frame; FrameContext::isFixedTickPhase
    // is true while inside the catch-up loop.
    virtual void Update(World& world, const FrameContext& ctx) = 0;

    // Default: marks the system as Opaque so the Scheduler serialises it
    // against every other system in the same phase. Override + call
    // Read/Write to opt INTO parallel batching.
    virtual void DeclareAccess(SystemAccessBuilder& builder) const
    {
        builder.OpaqueAccess();
    }
};

// CRTP-ish tag base: derive from SystemInPhase<TickPhase::X> to bake the
// phase into the type. SystemRegistry::Add asserts the runtime phase arg
// matches the static one.
template <TickPhase PHASE>
class SystemInPhase : public ISystem
{
public:
    static constexpr TickPhase kPhase = PHASE;
    TickPhase GetPhase() const override final { return PHASE; }
};
