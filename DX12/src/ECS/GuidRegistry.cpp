// GuidRegistry.cpp — Guid type generation/parsing + the global Guid↔Entity
// map used by AttachmentRef. See include/ECS/Guid.h, GuidComponent.h,
// GuidRegistry.h and DesignMd/entity_persistence_architecture.md.

#include "ECS/GuidRegistry.h"
#include "ECS/GuidComponent.h"
#include "ECS/ECS.h"
#include "System/Log.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>

// ===========================================================================
// Guid: generate / format / parse
// ===========================================================================
//
// Generate uses a thread-local mt19937_64 seeded once from random_device +
// the process start time. UUID v4 sets the version (4) and variant (10xx)
// nibbles so the bytes match RFC 4122 — handy when these IDs cross into
// external tools (asset DBs, profilers, save-game inspectors).

namespace ECS
{
    namespace
    {
        // mt19937_64 per thread to avoid lock contention on Generate. Each
        // thread mixes its tid into the seed so two threads booting in lock-
        // step never share a stream.
        thread_local std::mt19937_64* tls_rng = nullptr;

        std::mt19937_64& Rng()
        {
            if (tls_rng) return *tls_rng;
            static std::atomic<uint64_t> sCounter{ 0 };
            const uint64_t seed = std::random_device{}()
                ^ static_cast<uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())
                ^ (sCounter.fetch_add(1u, std::memory_order_relaxed) << 32);
            tls_rng = new std::mt19937_64(seed);
            return *tls_rng;
        }
    }

    Guid Guid::Generate()
    {
        auto& rng = Rng();
        Guid g;
        g.hi = rng();
        g.lo = rng();
        // RFC 4122 v4: top 4 bits of byte 6 = 0100; top 2 bits of byte 8 = 10.
        // hi/lo are big-endian conceptually, byte 6 sits in hi at bits 8-15
        // from the low end, byte 8 in lo at bits 56-63.
        g.hi = (g.hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
        g.lo = (g.lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
        // Avoid (1-in-2^128) all-zero output collapsing to invalid sentinel.
        if (!g.IsValid()) g.lo = 1u;
        return g;
    }

    std::string Guid::ToString() const
    {
        char buf[37];
        // Canonical UUID layout: 8-4-4-4-12. Decompose hi/lo into the 16
        // bytes in big-endian order so the printed string matches what most
        // tools expect.
        const uint32_t a = static_cast<uint32_t>(hi >> 32);
        const uint16_t b = static_cast<uint16_t>(hi >> 16);
        const uint16_t c = static_cast<uint16_t>(hi);
        const uint16_t d = static_cast<uint16_t>(lo >> 48);
        const uint64_t e = lo & 0x0000FFFFFFFFFFFFull;
        std::snprintf(buf, sizeof(buf),
            "%08x-%04x-%04x-%04x-%012llx",
            a, b, c, d, static_cast<unsigned long long>(e));
        return std::string(buf);
    }

    std::string Guid::ToHex() const
    {
        char buf[33];
        std::snprintf(buf, sizeof(buf),
            "%016llx%016llx",
            static_cast<unsigned long long>(hi),
            static_cast<unsigned long long>(lo));
        return std::string(buf);
    }

    namespace
    {
        // Returns -1 if c is not a hex digit.
        int HexNibble(char c) noexcept
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        }

        // Reads up to 32 hex chars from `s`, skipping dashes, returns true
        // on success. Fills hi (first 16 chars) and lo (next 16 chars).
        bool ParseHex(const char* s, uint64_t& hi, uint64_t& lo) noexcept
        {
            hi = 0u;
            lo = 0u;
            int consumed = 0;
            for (const char* p = s; *p && consumed < 32; ++p)
            {
                if (*p == '-') continue;
                const int n = HexNibble(*p);
                if (n < 0) return false;
                if (consumed < 16) hi = (hi << 4) | static_cast<uint64_t>(n);
                else               lo = (lo << 4) | static_cast<uint64_t>(n);
                ++consumed;
            }
            return consumed == 32;
        }
    }

    Guid Guid::Parse(const char* s)
    {
        if (!s) return kInvalidGuid;
        Guid g;
        if (!ParseHex(s, g.hi, g.lo)) return kInvalidGuid;
        return g;
    }

    Guid Guid::FromHex(const char* s)
    {
        if (!s) return kInvalidGuid;
        Guid g;
        if (!ParseHex(s, g.hi, g.lo)) return kInvalidGuid;
        return g;
    }

} // namespace ECS

// ===========================================================================
// GuidRegistry
// ===========================================================================

namespace ECS
{
    GuidRegistry& GuidRegistry::Get()
    {
        static GuidRegistry s;
        return s;
    }

    void GuidRegistry::Register(Guid guid, Entity e)
    {
        if (!guid.IsValid() || e == NullEntity) return;
        // Unregister any prior binding so the maps don't get out of sync.
        if (auto it = m_guidToEntity.find(guid); it != m_guidToEntity.end())
        {
            if (it->second == e) return;  // already registered
            m_entityToGuid.erase(it->second);
        }
        if (auto it = m_entityToGuid.find(e); it != m_entityToGuid.end())
        {
            m_guidToEntity.erase(it->second);
        }
        m_guidToEntity[guid] = e;
        m_entityToGuid[e]    = guid;
    }

    void GuidRegistry::Unregister(Guid guid)
    {
        auto it = m_guidToEntity.find(guid);
        if (it == m_guidToEntity.end()) return;
        m_entityToGuid.erase(it->second);
        m_guidToEntity.erase(it);
    }

    void GuidRegistry::UnregisterEntity(Entity e)
    {
        auto it = m_entityToGuid.find(e);
        if (it == m_entityToGuid.end()) return;
        m_guidToEntity.erase(it->second);
        m_entityToGuid.erase(it);
    }

    Entity GuidRegistry::Find(Guid guid) const noexcept
    {
        if (!guid.IsValid()) return NullEntity;
        auto it = m_guidToEntity.find(guid);
        return it != m_guidToEntity.end() ? it->second : NullEntity;
    }

    Guid GuidRegistry::ReverseFind(Entity e) const noexcept
    {
        if (e == NullEntity) return kInvalidGuid;
        auto it = m_entityToGuid.find(e);
        return it != m_entityToGuid.end() ? it->second : kInvalidGuid;
    }

    void GuidRegistry::Clear() noexcept
    {
        m_guidToEntity.clear();
        m_entityToGuid.clear();
    }

    void GuidRegistry::RegisterWithWorld(World& world)
    {
        if (m_hookedWorld == &world) return;
        if (m_hookedWorld) DetachFromWorld(*m_hookedWorld);

        m_hookedWorld = &world;
        m_listenerHandle = world.AddEntityDestroyListener(
            [this](Entity e) { UnregisterEntity(e); });
    }

    void GuidRegistry::DetachFromWorld(World& world)
    {
        if (m_hookedWorld != &world) return;
        if (m_listenerHandle != 0u)
            world.RemoveEntityDestroyListener(m_listenerHandle);
        m_listenerHandle = 0u;
        m_hookedWorld    = nullptr;
    }

    void GuidRegistry::RebuildFromWorld(World& world)
    {
        Clear();
        world.ForEach<GuidComponent>([&](Entity e, GuidComponent& gc)
        {
            if (!gc.guid.IsValid())
                gc.guid = Guid::Generate();  // shouldn't happen post-load but heal silently
            Register(gc.guid, e);
        });
    }

    Guid EnsureGuidOn(World& world, Entity e)
    {
        if (!world.IsAlive(e)) return kInvalidGuid;
        auto* gc = world.GetComponent<GuidComponent>(e);
        if (!gc)
        {
            GuidComponent newGc{};
            newGc.guid = Guid::Generate();
            world.AddComponent<GuidComponent>(e, newGc);
            GuidRegistry::Get().Register(newGc.guid, e);
            return newGc.guid;
        }
        if (!gc->guid.IsValid())
        {
            gc->guid = Guid::Generate();
        }
        // Make sure the registry knows even if the component pre-existed
        // without a registry entry (e.g. loaded from disk before
        // RebuildFromWorld ran).
        GuidRegistry::Get().Register(gc->guid, e);
        return gc->guid;
    }

} // namespace ECS

// ===========================================================================
// AttachmentRef::Resolve / Bind / BindAndStamp
// ===========================================================================
//
// Out-of-line here so GuidComponent.h doesn't need to include GuidRegistry.h
// or ECS.h's full World type — keeps the include graph thin.

Entity AttachmentRef::Resolve(World& world) const
{
    if (!ownerGuid.IsValid()) return NullEntity;

    // Steady-state fast path: cached entity is still alive AND its generation
    // matches what we cached. One branch + one array load.
    if (cachedOwner != NullEntity
        && world.IsAlive(cachedOwner)
        && world.GetGeneration(cachedOwner) == cachedGeneration)
    {
        return cachedOwner;
    }

    // Slow path: registry lookup. Update cache for the next call.
    const Entity e = ECS::GuidRegistry::Get().Find(ownerGuid);
    if (e == NullEntity)
    {
        cachedOwner      = NullEntity;
        cachedGeneration = 0u;
        return NullEntity;
    }
    cachedOwner      = e;
    cachedGeneration = world.GetGeneration(e);
    return e;
}

void AttachmentRef::Bind(World& world, Entity target)
{
    if (target == NullEntity)
    {
        Clear();
        return;
    }
    auto* gc = world.GetComponent<GuidComponent>(target);
    if (!gc)
    {
        // Caller didn't stamp the target first — leave the ref empty.
        // BindAndStamp is the auto-stamping variant.
        Clear();
        return;
    }
    ownerGuid        = gc->guid;
    cachedOwner      = target;
    cachedGeneration = world.GetGeneration(target);
}

ECS::Guid AttachmentRef::BindAndStamp(World& world, Entity target)
{
    if (target == NullEntity)
    {
        Clear();
        return ECS::kInvalidGuid;
    }
    const ECS::Guid g = ECS::EnsureGuidOn(world, target);
    ownerGuid        = g;
    cachedOwner      = target;
    cachedGeneration = world.GetGeneration(target);
    return g;
}
