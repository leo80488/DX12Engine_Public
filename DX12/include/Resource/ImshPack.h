#pragma once

// ImshPack — single-file archive of N `.imsh` mesh blobs.
//
// Problem this solves: loading Bistro-scale scenes (22k+ individual .imsh
// files) spends most of its wall-clock time in per-file OS overhead
// (~300 μs × 22k files ≈ 6-7 s of pure `fopen`, plus buffer handling).
//
// Format (all fields little-endian, byte-packed, no padding):
//   Header      (16 B)          : magic 'IMPK', version, fileCount, _pad
//   Entry table (N × 112 B)     : per-file name[96] + offset/size
//   Blob data   (concatenated)  : raw bytes of each .imsh blob, back-to-back
//
// The 96-byte name field is big enough for "exterior_Paris_BistroAwning_*.imsh"
// style filenames (commonly 40-60 chars). Longer paths are truncated by
// the builder — looked up via NormalizePath(basename) at runtime.
//
// Lookup is O(1) via a hash map built once when the pack is opened.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource
{
    constexpr uint32_t MAGIC_IMSH_PACK   = 'IMPK'; // 0x494D504B
    constexpr uint32_t IMSH_PACK_VERSION = 1;

#pragma pack(push, 1)
    struct ImshPackHeader
    {
        uint32_t magic;        // MAGIC_IMSH_PACK
        uint32_t version;      // IMSH_PACK_VERSION
        uint32_t fileCount;    // number of entries
        uint32_t _pad;         // reserved, 0
    };
    static_assert(sizeof(ImshPackHeader) == 16, "ImshPackHeader must be 16 bytes");

    struct ImshPackEntry
    {
        char     name[96];     // filename only (basename), null-terminated
        uint64_t offset;       // bytes from file start to the blob
        uint64_t size;         // length of the blob in bytes
    };
    static_assert(sizeof(ImshPackEntry) == 112, "ImshPackEntry must be 112 bytes");
#pragma pack(pop)

    // ---------------------------------------------------------------------
    // Reader — loads the full file into memory once, provides O(1) blob
    // lookup by filename. Suitable for ~100 MB-class archives; for larger
    // ones switch to memory-mapped IO (mmap / CreateFileMapping).
    // ---------------------------------------------------------------------
    class ImshPack
    {
    public:
        ImshPack() = default;

        // Open and index the archive. Returns false if file is missing or
        // has an invalid header.
        bool Open(const std::string& path);

        // Close and release memory.
        void Close();

        bool IsOpen() const { return !m_data.empty(); }

        // Lookup by basename (e.g. "exterior_Paris_Street_1_m0.imsh"). Returns
        // nullptr if the pack doesn't contain that file.
        const uint8_t* Lookup(const std::string& basename, size_t& outSize) const;

        size_t Count() const { return m_entries.size(); }

    private:
        std::vector<uint8_t> m_data;
        // basename → { offset, size }
        std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> m_entries;
    };

    // ---------------------------------------------------------------------
    // Builder — walks `folder` (non-recursive), gathers every `*.imsh`,
    // concatenates them into `outPath`. Returns false on any I/O error.
    // Skips existing `outPath` (returns true without rebuilding) if its
    // file count matches the current directory listing — lets the caller
    // call this unconditionally without re-packing on every launch.
    // ---------------------------------------------------------------------
    bool BuildImshPack(const std::string& folder, const std::string& outPath);
}
