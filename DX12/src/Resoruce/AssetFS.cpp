#include "Resource/AssetFS.h"
#include "System/Log.h"

#include <cstring>
#include <fstream>

namespace
{
    // Forward slashes everywhere. The .ipak index is built with normalised
    // keys, so ReadFile callers that hand in either "asset/foo" or "asset\foo"
    // hit the same entry.
    std::string Normalize(std::string s)
    {
        for (char& c : s) if (c == '\\') c = '/';
        while (s.size() >= 2 && s[0] == '.' && s[1] == '/')
            s.erase(0, 2);
        return s;
    }
}

namespace Resource
{
    AssetFS& AssetFS::Get()
    {
        static AssetFS s;
        return s;
    }

    bool AssetFS::Mount(const std::string& pakPath)
    {
        Unmount();

        std::ifstream f(pakPath, std::ios::binary | std::ios::ate);
        if (!f)
        {
            LOG_INFO("AssetFS: no pak at '%s' — running disk-only", pakPath.c_str());
            return false;
        }
        const std::streamsize fileSize = f.tellg();
        f.seekg(0, std::ios::beg);

        m_blob.resize(static_cast<std::size_t>(fileSize));
        if (!f.read(reinterpret_cast<char*>(m_blob.data()), fileSize))
        {
            LOG_ERROR("AssetFS: read failed on '%s'", pakPath.c_str());
            m_blob.clear();
            return false;
        }

        // Header: 4 bytes magic + u32 version + u32 entry count
        if (m_blob.size() < 12 ||
            m_blob[0] != 'I' || m_blob[1] != 'P' ||
            m_blob[2] != 'A' || m_blob[3] != 'K')
        {
            LOG_ERROR("AssetFS: '%s' is not a valid .ipak (bad magic)", pakPath.c_str());
            m_blob.clear();
            return false;
        }

        std::uint32_t version    = 0;
        std::uint32_t entryCount = 0;
        std::memcpy(&version,    m_blob.data() + 4, 4);
        std::memcpy(&entryCount, m_blob.data() + 8, 4);
        if (version != 1)
        {
            LOG_ERROR("AssetFS: '%s' version %u unsupported (expected 1)",
                      pakPath.c_str(), version);
            m_blob.clear();
            return false;
        }

        // Walk the index. Each entry is: u32 path_len | path bytes | u64 offset | u64 size
        std::size_t cursor = 12;
        m_index.reserve(entryCount);
        for (std::uint32_t i = 0; i < entryCount; ++i)
        {
            if (cursor + 4 > m_blob.size()) { LOG_ERROR("AssetFS: truncated index"); m_blob.clear(); m_index.clear(); return false; }
            std::uint32_t pathLen = 0;
            std::memcpy(&pathLen, m_blob.data() + cursor, 4);
            cursor += 4;
            if (cursor + pathLen + 16 > m_blob.size()) { LOG_ERROR("AssetFS: truncated entry %u", i); m_blob.clear(); m_index.clear(); return false; }

            std::string path(reinterpret_cast<const char*>(m_blob.data() + cursor), pathLen);
            cursor += pathLen;

            Entry e{};
            std::memcpy(&e.offset, m_blob.data() + cursor, 8);   cursor += 8;
            std::memcpy(&e.size,   m_blob.data() + cursor, 8);   cursor += 8;

            if (e.offset + e.size > m_blob.size())
            {
                LOG_ERROR("AssetFS: entry '%s' offset+size (%llu) exceeds pak size (%zu)",
                          path.c_str(),
                          static_cast<unsigned long long>(e.offset + e.size),
                          m_blob.size());
                m_blob.clear();
                m_index.clear();
                return false;
            }
            m_index.emplace(Normalize(std::move(path)), e);
        }

        LOG_INFO("AssetFS: mounted '%s' — %zu entries, %.1f MiB",
                 pakPath.c_str(), m_index.size(),
                 m_blob.size() / (1024.0 * 1024.0));
        return true;
    }

    void AssetFS::Unmount()
    {
        m_index.clear();
        m_blob.clear();
        m_blob.shrink_to_fit();
    }

    bool AssetFS::HasInPak(const std::string& path) const
    {
        if (m_index.empty()) return false;
        return m_index.find(Normalize(path)) != m_index.end();
    }

    bool AssetFS::ReadFile(const std::string& path, std::vector<std::uint8_t>& out) const
    {
        // Pak path — zero-allocation except for the final output copy.
        if (!m_index.empty())
        {
            const auto it = m_index.find(Normalize(path));
            if (it != m_index.end())
            {
                out.resize(static_cast<std::size_t>(it->second.size));
                std::memcpy(out.data(),
                            m_blob.data() + it->second.offset,
                            static_cast<std::size_t>(it->second.size));
                return true;
            }
        }

        // Disk fallback — dev workflow, or assets added after packing.
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        const std::streamsize sz = f.tellg();
        f.seekg(0, std::ios::beg);
        out.resize(static_cast<std::size_t>(sz));
        return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), sz));
    }

    bool AssetFS::ReadFileText(const std::string& path, std::string& out) const
    {
        std::vector<std::uint8_t> bytes;
        if (!ReadFile(path, bytes)) return false;
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    void AssetFS::EnumerateUnder(const std::string& prefix,
                                 std::vector<std::string>& out) const
    {
        // No-op when no pak is mounted — callers fall back to a loose-disk scan.
        const std::string pfx = Normalize(prefix);
        for (const auto& kv : m_index)
            if (kv.first.size() >= pfx.size() &&
                kv.first.compare(0, pfx.size(), pfx) == 0)
                out.push_back(kv.first);
    }
}
