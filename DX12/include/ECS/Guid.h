#pragma once

// Guid — 128-bit identifier used as the stable cross-session identity for
// entities that need to survive scene reload / save game. See
// DesignMd/entity_persistence_architecture.md §3.2.
//
// Why 128-bit:
//   - Birthday-collision probability stays cosmologically small (1 in 2^64
//     after ~2.7×10^9 IDs) so we never have to deduplicate.
//   - Standard UUID v4 wire format fits — every external tool can read it.
//
// Cheap-to-pass — two uint64_t, trivially copyable. No allocation. Compares
// in two register cmps. Hashable via std::hash specialization at the bottom.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace ECS
{

struct Guid
{
    // hi/lo are stored in raw big-endian conceptually; toString prints the
    // canonical 8-4-4-4-12 UUID layout so it round-trips with external GUID
    // tools. Internally we treat hi/lo as opaque random bits.
    uint64_t hi = 0u;
    uint64_t lo = 0u;

    constexpr bool IsValid() const noexcept { return (hi | lo) != 0u; }

    constexpr bool operator==(const Guid& o) const noexcept
    { return hi == o.hi && lo == o.lo; }
    constexpr bool operator!=(const Guid& o) const noexcept
    { return !(*this == o); }
    // Ordering only for std::map convenience; semantically Guids are not
    // ordered. Compare lexicographically by (hi,lo).
    constexpr bool operator<(const Guid& o) const noexcept
    { return hi != o.hi ? hi < o.hi : lo < o.lo; }

    // Generate a new random Guid. UUID v4-ish — sets variant/version nibbles
    // so it round-trips through tools expecting RFC 4122. Thread-safe.
    static Guid Generate();

    // Canonical 36-char UUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
    std::string ToString() const;

    // Parse a 36-char UUID string. Returns invalid Guid (hi=lo=0) on bad input.
    // Accepts upper or lower hex; dashes optional.
    static Guid Parse(const char* s);
    static Guid Parse(const std::string& s) { return Parse(s.c_str()); }

    // Compact 32-char hex (no dashes) — used for the .iscn key=value text
    // format so we don't have to quote spaces or escape dashes.
    std::string ToHex() const;
    static Guid FromHex(const char* s);
    static Guid FromHex(const std::string& s) { return FromHex(s.c_str()); }
};

inline constexpr Guid kInvalidGuid{ 0u, 0u };

} // namespace ECS

// std::hash specialization so Guid is a valid unordered_map key without the
// caller writing a hasher.
namespace std
{
    template<> struct hash<ECS::Guid>
    {
        size_t operator()(const ECS::Guid& g) const noexcept
        {
            // FNV-1a-ish mix of hi and lo. Both halves are already random,
            // so this is mostly XOR + a couple of multiplies to spread bits
            // — no real avalanche needed.
            uint64_t h = g.hi ^ (g.lo + 0x9e3779b97f4a7c15ull
                                + (g.hi << 6) + (g.hi >> 2));
            return static_cast<size_t>(h);
        }
    };
}
