#pragma once

// AssetFS — read-only virtual filesystem with optional .ipak mount.
//
// Binary format (little-endian throughout; written by tools/pack_assets.py):
//
//   [0..4)     magic "IPAK"
//   [4..8)     version u32 (currently 1)
//   [8..12)    entry count u32
//   [12..)     index section — for each entry:
//                 path_len u32
//                 path     path_len bytes (UTF-8, no null terminator)
//                 offset   u64 (absolute byte offset into this .ipak file)
//                 size     u64
//   [blob]     raw concatenated file bytes, referenced by (offset,size) tuples.
//
// Runtime behaviour:
//   - Mount() slurps the whole .ipak into memory and builds a path → (offset,size)
//     hash map. Paths are normalised to forward slashes at index build time.
//   - ReadFile(path, out) looks up the normalised path in the pak first. Falls
//     back to a plain disk read if not present (or if no pak is mounted).
//     This makes development in the editor — where game.ipak is absent and
//     assets live as loose files — work without any call-site changes.
//   - Thread-safety: Mount must complete before any ReadFile calls. Reads are
//     pure look-ups + memcpy from an immutable buffer, so concurrent ReadFiles
//     are safe without locks.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource
{
    class AssetFS
    {
    public:
        static AssetFS& Get();

        // Load `pakPath` into memory and build the lookup index.
        // Returns false if the file does not exist or is malformed; the caller
        // decides whether to treat that as fatal (Game.exe: yes if missing;
        // Editor.exe: no — it's expected to be absent during development).
        bool Mount(const std::string& pakPath);

        // Release the mounted blob (if any). Subsequent ReadFile calls fall
        // back to disk only.
        void Unmount();

        bool IsMounted() const { return !m_blob.empty(); }
        std::size_t PakEntryCount() const { return m_index.size(); }

        // True if `path` (after slash normalisation) is present in the mounted
        // pak. False if no pak is mounted or the path is disk-only.
        bool HasInPak(const std::string& path) const;

        // Read a file into `out`. Prefers the pak when mounted; falls back to
        // std::ifstream reading from the current working directory. Returns
        // false only if neither source has the file.
        bool ReadFile(const std::string& path, std::vector<uint8_t>& out) const;

        // Convenience wrapper around ReadFile that returns the bytes as a
        // std::string — used by the Lua loaders (run via sol::state::safe_script
        // on the source string) so scripts resolve from the pak in a packed
        // build instead of being read straight off disk.
        bool ReadFileText(const std::string& path, std::string& out) const;

        // Append every mounted-pak entry whose normalised (forward-slash) path
        // begins with `prefix` to `out`. No-op when no pak is mounted. Used to
        // DISCOVER directory-scanned assets (e.g. asset/scripts/systems/*.lua) in
        // packed builds, where there are no loose files for a directory_iterator
        // to enumerate. Content is still read via ReadFile/ReadFileText.
        void EnumerateUnder(const std::string& prefix, std::vector<std::string>& out) const;

    private:
        struct Entry { std::uint64_t offset; std::uint64_t size; };

        std::vector<std::uint8_t>                m_blob;   // whole .ipak
        std::unordered_map<std::string, Entry>   m_index;
    };
}
