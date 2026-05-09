#pragma once

// AudioClipSystem — path-deduplication cache for .aclip resources.
// Mirrors AnimationClipSystem 1:1.
//
//   - AcquireClip(path)     → AudioHandle (ref-counted, same path → same handle)
//   - ReleaseClip(handle)   → refCount--; auto-Unload at zero
//   - IsReady(handle)       → true once ResourceManager finishes loading
//   - GetResource(handle)   → const AudioClipResource* (borrowed; do NOT store)
//   - Tick()                → promote ResourceManager-ready clips to local Ready
//
// AudioSystem calls AcquireClip(clipPath) the first time it sees a path,
// stores the resulting handle on the AudioSourceComponent, then re-checks
// IsReady before invoking AudioEngine::PlayClip.

#include "Resource/ResourceManager.h"
#include "Resource/SystemHandles.h"   // AudioHandle alias
#include <mutex>
#include <string>
#include <unordered_map>

namespace Audio
{
    class AudioClipResource;

    class AudioClipSystem
    {
    public:
        AudioClipSystem()  = default;
        ~AudioClipSystem() = default;

        AudioClipSystem(const AudioClipSystem&)            = delete;
        AudioClipSystem& operator=(const AudioClipSystem&) = delete;

        void Init(Resource::ResourceManager& rm);
        void Shutdown();

        // ---- Async load + dedup ------------------------------------------------

        Resource::AudioHandle AcquireClip(const std::string& path);
        void                  ReleaseClip(Resource::AudioHandle handle);

        bool                       IsReady    (Resource::AudioHandle handle) const;
        const AudioClipResource*   GetResource(Resource::AudioHandle handle) const;

        // ---- Per-frame pump ----------------------------------------------------
        // Promotes RM-ready entries to local Ready. Idempotent; cheap to skip.
        void Tick();

    private:
        static std::string NormalizePath(const std::string& path);

        enum class ClipState { Loading, Ready };

        struct ClipEntry
        {
            Resource::AudioHandle handle;
            Resource::Handle      rmHandle;
            ClipState             state    = ClipState::Loading;
            uint32_t              refCount = 0;
        };

        Resource::ResourceManager*                 m_rm = nullptr;
        mutable std::mutex                         m_mutex;
        uint32_t                                   m_nextSlot = 1;
        std::unordered_map<std::string, ClipEntry> m_byPath;
        std::unordered_map<uint32_t, std::string>  m_pathBySlot;
    };
}
