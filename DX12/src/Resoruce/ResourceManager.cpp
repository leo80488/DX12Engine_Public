#include "Resource/ResourceManager.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cassert>
#include <filesystem>

namespace Resource
{
    // =========================================================================
    //  Construction
    // =========================================================================

    ResourceManager::ResourceManager()
    {
        m_slots.reserve(4096);
        m_freeList.reserve(256);
    }

    ResourceManager::~ResourceManager()
    {
        // Signal that no new loads may be queued.
        m_shuttingDown.store(true, std::memory_order_release);

        // Block until every in-flight ProcessOneLoad task has exited.
        // Without this, worker-thread lambdas that captured `this` can run after
        // member destructors fire (particularly m_loaderMap / m_mutex), causing UB.
        std::unique_lock<std::mutex> lk(m_drainMutex);
        m_drainCV.wait(lk, [this]
        {
            return m_activeLoads.load(std::memory_order_acquire) == 0;
        });
    }

    // =========================================================================
    //  Importer / Loader registry
    // =========================================================================

    void ResourceManager::BuildImporterMap()
    {
        m_importerMap.clear();
        for (const auto& imp : m_importers)
        {
            if (!imp) continue;
            for (const char* ext : imp->GetSourceExtensions())
                if (ext) m_importerMap[ext] = imp.get();
        }
    }

    void ResourceManager::RegisterImporter(std::shared_ptr<IImporter> importer)
    {
        if (!importer) return;
        std::lock_guard lock(m_mutex);
        m_importers.push_back(std::move(importer));
        BuildImporterMap();
    }

    void ResourceManager::BuildLoaderMap()
    {
        m_loaderMap.clear();
        for (const auto& loader : m_loaders)
            if (loader && loader->GetExtension())
                m_loaderMap[loader->GetExtension()] = loader.get();
    }

    void ResourceManager::RegisterLoader(std::shared_ptr<IResourceLoader> loader)
    {
        if (!loader) return;
        std::lock_guard lock(m_mutex);
        m_loaders.push_back(std::move(loader));
        BuildLoaderMap();
    }

    // =========================================================================
    //  Path utilities
    // =========================================================================

    std::string ResourceManager::NormalizePath(const std::string& path) const
    {
        std::string s = path;
        for (char& c : s) if (c == '\\') c = '/';
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }

    std::string ResourceManager::GetExtension(const std::string& path) const
    {
        size_t pos = path.find_last_of('.');
        if (pos == std::string::npos) return {};
        std::string ext = path.substr(pos);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext;
    }

    std::string ResourceManager::DeriveCachePath(const std::string& sourcePath,
        const char* internalExtension)
    {
        size_t dot = sourcePath.find_last_of('.');
        if (dot == std::string::npos) return sourcePath + internalExtension;
        return sourcePath.substr(0, dot) + internalExtension;
    }

    bool ResourceManager::FileExists(const std::string& path)
    {
        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }

    uint64_t ResourceManager::HashPath(const std::string& path)
    {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : path) { hash ^= c; hash *= 1099511628211ULL; }
        return hash;
    }

    IImporter* ResourceManager::FindImporter(const std::string& ext) const
    {
        auto it = m_importerMap.find(ext);
        return it != m_importerMap.end() ? it->second : nullptr;
    }

    IResourceLoader* ResourceManager::FindLoader(const std::string& ext) const
    {
        auto it = m_loaderMap.find(ext);
        return it != m_loaderMap.end() ? it->second : nullptr;
    }

    // =========================================================================
    //  Slot pool  (callers hold m_mutex)
    // =========================================================================

    uint32_t ResourceManager::AllocSlot()
    {
        if (!m_freeList.empty())
        {
            uint32_t idx = m_freeList.back();
            m_freeList.pop_back();
            return idx;
        }
        m_slots.emplace_back();
        return static_cast<uint32_t>(m_slots.size() - 1);
    }

    void ResourceManager::FreeSlot(uint32_t index)
    {
        assert(index < m_slots.size());
        ResourceEntry& e = m_slots[index];

        m_pathHashToSlot.erase(HashPath(e.path));

        // Bump generation — invalidates ALL existing Handles pointing at this slot.
        // Wrap around but never land on 0 (0 is the null-handle sentinel).
        e.generation = (e.generation % Handle::MAX_GEN) + 1;

        // Release ownership immediately — no shared_ptr means truly gone.
        e.resource.reset();
        e.rawData.clear();
        e.rawData.shrink_to_fit();
        e.state = ResourceState::NotLoaded;
        e.path.clear();
        e.type = ResourceType::Unknown;
        e.handle = Handle{};

        m_freeList.push_back(index);
    }

    bool ResourceManager::IsValid(Handle h) const
    {
        if (!h.IsValid()) return false;
        uint32_t idx = h.Index();
        if (idx >= m_slots.size()) return false;
        // Generation mismatch → stale handle, slot may already be reused.
        return m_slots[idx].generation == h.Generation();
    }

    // =========================================================================
    //  Load
    // =========================================================================

    Handle ResourceManager::Load(const std::string& path, ResourceType type, int priority)
    {
        std::string norm = NormalizePath(path);
        if (norm.empty()) return Handle{};

        const uint64_t key = HashPath(norm);
        std::lock_guard lock(m_mutex);

        auto it = m_pathHashToSlot.find(key);
        if (it != m_pathHashToSlot.end())
        {
            uint32_t       idx = it->second;
            ResourceEntry& entry = m_slots[idx];

            if (entry.state == ResourceState::Loading) return entry.handle;
            if (entry.state == ResourceState::Ready && entry.resource) return entry.handle;

            // Stale / Failed → recycle the slot (generation is bumped inside FreeSlot).
            FreeSlot(idx);
        }

        uint32_t       idx = AllocSlot();
        ResourceEntry& entry = m_slots[idx];

        if (entry.generation == 0) entry.generation = 1; // never issue gen=0 handles

        entry.path = norm;
        entry.type = type;
        entry.state = ResourceState::Loading;
        entry.handle = Handle::Make(idx, type, static_cast<uint16_t>(entry.generation));

        m_pathHashToSlot[key] = idx;

        // Do not queue new loads after shutdown has started.
        if (m_shuttingDown.load(std::memory_order_acquire))
            return Handle{};

        const TaskSystem::TaskPriority tp =
            priority > 0 ? TaskSystem::TaskPriority::High
            : TaskSystem::TaskPriority::Low;

        // Increment before Push so the destructor can never see zero while a
        // task is queued but not yet running.
        m_activeLoads.fetch_add(1, std::memory_order_relaxed);

        TaskSystem::Get().Push([this, idx, norm]()
        {
            ProcessOneLoad(idx, norm);

            // Decrement and wake the drain wait in the destructor.
            if (m_activeLoads.fetch_sub(1, std::memory_order_acq_rel) == 1)
                m_drainCV.notify_all();
        }, tp);

        return entry.handle;
    }

    // =========================================================================
    //  Unload  (explicit eviction)
    // =========================================================================

    void ResourceManager::Unload(Handle handle)
    {
        if (!handle.IsValid()) return;
        std::lock_guard lock(m_mutex);
        if (!IsValid(handle)) return;  // already stale, nothing to do
        FreeSlot(handle.Index());
        // After this call, every existing Handle with the old generation is stale.
        // Callers that stored the Handle will get NotLoaded / nullptr next query.
    }

    // =========================================================================
    //  Queries  (O(1))
    // =========================================================================

    ResourceState ResourceManager::GetState(Handle handle) const
    {
        if (!handle.IsValid()) return ResourceState::NotLoaded;
        std::lock_guard lock(m_mutex);
        if (!IsValid(handle)) return ResourceState::NotLoaded;
        const ResourceEntry& e = m_slots[handle.Index()];
        if (e.state == ResourceState::Ready && !e.resource) return ResourceState::NotLoaded;
        return e.state;
    }

    // Returns a RAW BORROWED pointer.  Valid only while the caller holds no
    // lock that would permit another thread to call Unload() or Shutdown().
    // In practice: use the pointer within the same game-loop tick, then discard.
    const Resource* ResourceManager::GetResource(Handle handle) const
    {
        if (!handle.IsValid()) return nullptr;
        std::lock_guard lock(m_mutex);
        if (!IsValid(handle)) return nullptr;
        return m_slots[handle.Index()].resource.get();
    }

    std::string ResourceManager::GetPath(Handle handle) const
    {
        if (!handle.IsValid()) return {};
        std::lock_guard lock(m_mutex);
        if (!IsValid(handle)) return {};
        return m_slots[handle.Index()].path;
    }

    const std::vector<uint8_t>* ResourceManager::GetRawData(Handle handle) const
    {
        if (!handle.IsValid()) return nullptr;
        std::lock_guard lock(m_mutex);
        if (!IsValid(handle)) return nullptr;
        const ResourceEntry& e = m_slots[handle.Index()];
        if (e.state != ResourceState::Ready) return nullptr;
        return &e.rawData;
    }

    // =========================================================================
    //  Background worker
    // =========================================================================

    void ResourceManager::ProcessOneLoad(uint32_t slotIndex, std::string sourcePath)
    {
        auto readFile = [](const std::string& p, std::vector<uint8_t>& out) -> bool
            {
                return ::Resource::AssetFS::Get().ReadFile(p, out);
            };

        auto failSlot = [&](const char* reason)
            {
                LOG_ERROR("ResourceManager: %s (path: '%s')", reason, sourcePath.c_str());
                std::lock_guard lock(m_mutex);
                if (slotIndex < m_slots.size())
                    m_slots[slotIndex].state = ResourceState::Failed;
            };

        const std::string srcExt = GetExtension(sourcePath);
        std::string       loadPath;
        std::vector<uint8_t> internalData;

        // --- Decide import vs direct load (brief lock) ---
        bool needsImport = false;
        {
            std::lock_guard lock(m_mutex);
            IImporter* imp = FindImporter(srcExt);
            if (imp)
            {
                const std::string cachePath = DeriveCachePath(sourcePath, imp->GetInternalExtension());
                if (FileExists(cachePath)) { loadPath = cachePath; }
                else { loadPath = cachePath; needsImport = true; }
            }
            else
            {
                loadPath = sourcePath;
            }
        }

        // --- File I/O (no lock held) ---
        if (needsImport)
        {
            std::vector<uint8_t> sourceData;
            if (!readFile(sourcePath, sourceData)) { failSlot("failed to open source file"); return; }

            IImporter* imp = nullptr;
            { std::lock_guard lock(m_mutex); imp = FindImporter(srcExt); }

            internalData = imp->Import(sourcePath, sourceData);
            if (internalData.empty()) { failSlot("importer returned empty blob"); return; }

            // Write cache (best-effort)
            try
            {
                std::filesystem::path fp(loadPath);
                if (fp.has_parent_path())
                {
                    std::error_code ec; std::filesystem::create_directories(fp.parent_path(), ec);
                }
                if (std::ofstream out(loadPath, std::ios::binary); out)
                    out.write(reinterpret_cast<const char*>(internalData.data()),
                        static_cast<std::streamsize>(internalData.size()));
                else
                    LOG_WARNING("ResourceManager: could not write cache '%s'", loadPath.c_str());
            }
            catch (...) { LOG_WARNING("ResourceManager: exception writing cache '%s'", loadPath.c_str()); }
        }
        else
        {
            if (!readFile(loadPath, internalData)) { failSlot("failed to read internal file"); return; }
        }

        // --- Dispatch to loader (no lock held) ---
        const std::string internalExt = GetExtension(loadPath);
        IResourceLoader* loader = nullptr;
        { std::lock_guard lock(m_mutex); loader = FindLoader(internalExt); }

        if (!loader) { failSlot("no loader for internal extension"); return; }

        LoadResult result;
        try { result = loader->Load(loadPath, internalData); }
        catch (const std::exception& ex)
        {
            LOG_ERROR("ResourceManager: loader threw for '%s': %s", loadPath.c_str(), ex.what());
            std::lock_guard lock(m_mutex);
            if (slotIndex < m_slots.size()) m_slots[slotIndex].state = ResourceState::Failed;
            return;
        }

        if (!result.resource) { failSlot("loader returned null resource"); return; }

        // --- Commit (unique_ptr move, no refcount) ---
        {
            std::lock_guard lock(m_mutex);
            if (slotIndex < m_slots.size())
            {
                ResourceEntry& e = m_slots[slotIndex];
                // Transfer ownership into the slot — clean single-owner transfer.
                e.resource = std::move(result.resource);
                e.rawData = std::move(internalData);
                e.state = ResourceState::Ready;
            }
            // If slotIndex is out of range the slot was recycled while loading (very rare
            // race: Unload() called between Load() and ProcessOneLoad() finishing).
            // result.resource falls out of scope here and is destroyed cleanly.
        }

        if (result.pendingUpload)
        {
            std::lock_guard uploadLock(m_uploadMutex);
            m_pendingUploads.push(std::move(result.pendingUpload));
        }
    }

    // =========================================================================
    //  GPU upload pump  (main thread)
    // =========================================================================

    void ResourceManager::ProcessPendingGPUUploads(float maxMs)
    {
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::duration<float, std::milli>(maxMs);

        std::queue<std::unique_ptr<IPendingGPUUpload>> local;
        { std::lock_guard lock(m_uploadMutex); local.swap(m_pendingUploads); }

        while (!local.empty() && Clock::now() < deadline)
        {
            auto task = std::move(local.front()); local.pop();
            if (task) task->Execute();
        }

        if (!local.empty())
        {
            std::lock_guard lock(m_uploadMutex);
            while (!local.empty())
            {
                m_pendingUploads.push(std::move(local.front())); local.pop();
            }
        }
    }

    // =========================================================================
    //  Shutdown
    // =========================================================================

    void ResourceManager::Shutdown()
    {
        // Prevent new loads from being queued.
        m_shuttingDown.store(true, std::memory_order_release);

        std::lock_guard lock(m_mutex);
        m_slots.clear();       // unique_ptr destructors run here — no leaked objects
        m_freeList.clear();
        m_pathHashToSlot.clear();
    }

} // namespace Resource