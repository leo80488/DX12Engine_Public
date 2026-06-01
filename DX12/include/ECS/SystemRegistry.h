#pragma once

// SystemRegistry — owns every ISystem, partitioned by TickPhase.
//
// Populated once at engine startup (see SystemRegistry::RegisterAllSystems
// in src/ECS/SystemRegistry.cpp — the SINGLE place in the engine where
// tick order is declared). The Scheduler queries by phase at Tick time.

#include "ISystem.h"

#include <array>
#include <cassert>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

class World;

namespace detail
{
    template <typename T, typename = void>
    struct HasStaticPhase : std::false_type {};

    template <typename T>
    struct HasStaticPhase<T, std::void_t<decltype(T::kPhase)>> : std::true_type {};
}

class SystemRegistry
{
public:
    // Construct an ISystem in place and stash it in `phase`'s bucket.
    // Returns a reference so callers that need to wire follow-up state
    // (event listeners, World cached pointer) don't have to look it up.
    //
    // If TSystem derives from SystemInPhase<P>, P must equal the runtime
    // `phase` arg — a runtime assert catches drift between the tag and
    // the registration call.
    template <typename TSystem, typename... Args>
    TSystem& Add(TickPhase phase, Args&&... args)
    {
        static_assert(std::is_base_of_v<ISystem, TSystem>,
                      "TSystem must derive from ISystem");
        if constexpr (detail::HasStaticPhase<TSystem>::value)
        {
            assert(TSystem::kPhase == phase &&
                   "SystemRegistry::Add — phase arg disagrees with "
                   "SystemInPhase<P> tag on the system type");
        }
        auto sys = std::make_unique<TSystem>(std::forward<Args>(args)...);
        TSystem& ref = *sys;
        m_phases[static_cast<size_t>(phase)].push_back(std::move(sys));
        ++m_generation;
        return ref;
    }

    // Overload for systems whose phase is fully baked into the type via
    // SystemInPhase<P>. Call sites read cleaner: Add<FooSystem>(args).
    template <typename TSystem, typename... Args>
        requires detail::HasStaticPhase<TSystem>::value
    TSystem& Add(Args&&... args)
    {
        return Add<TSystem>(TSystem::kPhase, std::forward<Args>(args)...);
    }

    // OnRegister every system in registration / phase order. Call after
    // World is constructed and before the first Tick.
    void Initialize(World& world);

    // OnUnregister in reverse order. Call before World::Clear during
    // engine shutdown — systems may hold listener handles or cached
    // pool pointers that need to detach while the World is still alive.
    void Shutdown(World& world);

    // Used by Scheduler::RunPhase. Span hides the underlying vector type
    // and forbids the caller from mutating membership at tick time.
    std::span<const std::unique_ptr<ISystem>> GetSystems(TickPhase phase) const;

    // Composition counter — bumped on every Add. Scheduler (S3+) uses
    // this to invalidate cached parallel-batch plans without having to
    // diff system lists every frame.
    uint64_t GetGeneration() const { return m_generation; }

private:
    std::array<std::vector<std::unique_ptr<ISystem>>,
               static_cast<size_t>(TickPhase::COUNT)> m_phases;
    uint64_t m_generation = 1;
};
