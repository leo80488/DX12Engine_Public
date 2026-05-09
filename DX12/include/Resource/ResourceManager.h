#pragma once

#include "Resource/Resource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/IResourceLoader.h"
#include "Resource/IImporter.h"
#include "Resource/IPendingGPUUpload.h"
#include "System/TaskSystem.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource
{
    enum class ResourceState
    {
        NotLoaded,
        Loading,
        Ready,
        Failed
    };

    // -------------------------------------------------------------------------
    //  ResourceEntry  —  one slot in the flat pool
    //
    //  Ownership model (THE KEY CHANGE vs the old code):
    //
    //    • resource is unique_ptr, not shared_ptr.
    //      ResourceManager is the sole owner.  Callers never hold a Resource*
    //      across frames — they ask ResourceManager for it each time.
    //
    //    • generation is bumped in FreeSlot().  Any Handle whose generation
    //      field no longer matches this slot is stale and will return nullptr /
    //      NotLoaded from every public API — no UB, no use-after-free.
    //
    //  Why NOT shared_ptr here?
    //    If callers cache a shared_ptr<Resource>, the object stays alive even
    //    after FreeSlot() bumps the generation.  The Handle becomes a
    //    misleading facade — stale handles look invalid, but the object is
    //    still referenced and memory is never released until the last external
    //    holder drops it.  That defeats the purpose of generational indexing.
    // -------------------------------------------------------------------------
    struct ResourceEntry
    {
        Handle                   handle;
        std::unique_ptr<Resource> resource;   // OWNED exclusively by ResourceManager
        ResourceState            state = ResourceState::NotLoaded;
        std::string              path;
        std::vector<uint8_t>     rawData;
        uint32_t                 generation = 0;
        ResourceType             type = ResourceType::Unknown;
    };

    class ResourceManager
    {
    public:
        ResourceManager();
        ~ResourceManager();

        ResourceManager(const ResourceManager&) = delete;
        ResourceManager& operator=(const ResourceManager&) = delete;

        void RegisterImporter(std::shared_ptr<IImporter> importer);
        void RegisterLoader(std::shared_ptr<IResourceLoader> loader);

        // -----------------------------------------------------------------------
        //  Load — asynchronous.  Returns a Handle immediately.
        //
        //  External path flow (importer registered for extension):
        //    1. Derive cache path (same dir, internal extension).
        //    2. Cache hit  → load cache directly.
        //    3. Cache miss → read source → Import() → write cache → load cache.
        //
        //  Internal path flow (no importer for extension):
        //    Load the file directly via the matching IResourceLoader.
        //
        //  Deduplication: Loading/Ready paths return the existing Handle.
        //  Stale/Failed entries are recycled (generation bumped).
        // -----------------------------------------------------------------------
        Handle Load(const std::string& path, ResourceType type = ResourceType::Unknown, int priority = 0);

        // -----------------------------------------------------------------------
        //  Queries — all O(1).
        //
        //  IMPORTANT: the raw pointer returned by GetResource() is a BORROWED
        //  reference valid only for the current call stack while m_mutex is not
        //  re-acquired.  Do NOT store it.  Use the Handle to re-query next frame.
        //
        //  If you need to pass the resource to another system in the same frame,
        //  pass the Handle — the receiving system calls GetResource() itself.
        // -----------------------------------------------------------------------
        ResourceState GetState(Handle handle) const;
        const Resource* GetResource(Handle handle) const; // borrowed, do NOT store
        std::string     GetPath(Handle handle) const;

        // Typed accessor — casts internally, returns nullptr on type mismatch or stale handle.
        template<typename T>
        const T* Get(Handle handle) const
        {
            return static_cast<const T*>(GetResource(handle));
        }

        // Raw internal-format bytes (available once state == Ready).
        const std::vector<uint8_t>* GetRawData(Handle handle) const;

        // Free a slot immediately.  All existing Handles to this slot become stale.
        // Safe to call from any thread while not iterating over resources.
        void Unload(Handle handle);

        // Main-thread GPU upload pump; maxMs is the per-frame time budget.
        void ProcessPendingGPUUploads(float maxMs = 2.0f);

        void Shutdown();

    private:
        void ProcessOneLoad(uint32_t slotIndex, std::string sourcePath);

        std::string      NormalizePath(const std::string& path) const;
        std::string      GetExtension(const std::string& path) const;
        static std::string DeriveCachePath(const std::string& sourcePath,
            const char* internalExtension);
        static bool        FileExists(const std::string& path);
        static uint64_t    HashPath(const std::string& path);

        IImporter* FindImporter(const std::string& extension) const;
        IResourceLoader* FindLoader(const std::string& extension) const;
        void BuildLoaderMap();
        void BuildImporterMap();

        // Slot pool helpers — callers hold m_mutex.
        uint32_t AllocSlot();
        void     FreeSlot(uint32_t index);   // bumps generation, resets entry
        bool     IsValid(Handle h) const;   // generation check only

        // ---- In-flight task drain (destructor waits for these to hit zero) -----
        // Prevents use-after-free when worker lambdas outlive the ResourceManager.
        std::atomic<int>        m_activeLoads{0};
        std::atomic<bool>       m_shuttingDown{false};
        std::condition_variable m_drainCV;
        std::mutex              m_drainMutex;   // used only with m_drainCV

        mutable std::mutex m_mutex;

        std::vector<ResourceEntry>             m_slots;
        std::vector<uint32_t>                  m_freeList;
        std::unordered_map<uint64_t, uint32_t> m_pathHashToSlot;

        std::vector<std::shared_ptr<IImporter>>           m_importers;
        std::unordered_map<std::string, IImporter*>       m_importerMap;

        std::vector<std::shared_ptr<IResourceLoader>>     m_loaders;
        std::unordered_map<std::string, IResourceLoader*> m_loaderMap;

        std::mutex                                         m_uploadMutex;
        std::queue<std::unique_ptr<IPendingGPUUpload>>     m_pendingUploads;
    };
}