#pragma once

// AnimationClipSystem — path-deduplication cache for .ianim animation resources.
//
// Mirrors the TextureSystem pattern:
//   - AcquireClip(path)      → AnimHandle (ref-counted, same path → same handle)
//   - ReleaseClip(handle)    → refCount--; auto-unload at zero
//   - IsReady(handle)        → true once ResourceManager finishes loading
//   - GetResource(handle)    → const AnimationResource* (borrowed, do NOT store)
//   - Tick()                 → promotes ResourceManager-loaded clips to Ready
//
// Skeleton binding:
//   - BindToSkeleton(handle, skeleton, clipLib)
//       → loads, binds all clips in the resource to the given skeleton,
//          registers them in clipLib, returns first ClipLibrary index
//       → returns kInvalidClipIndex if not yet ready

#include "Resource/SystemHandles.h"
#include "Resource/ResourceManager.h"
#include "Resource/AnimationResource.h"
#include "Resource/SkeletonAsset.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class World;
class Renderer;

namespace Resource
{
    class AnimationClipSystem
    {
    public:
        AnimationClipSystem()  = default;
        ~AnimationClipSystem() = default;

        AnimationClipSystem(const AnimationClipSystem&)            = delete;
        AnimationClipSystem& operator=(const AnimationClipSystem&) = delete;

        void Init(ResourceManager& rm);

        // ---- Async load + dedup ---------------------------------------------

        // Start async load; returns a handle immediately.
        // Same path → same handle, refCount++.
        // Call IsReady() to check completion before GetResource().
        AnimHandle AcquireClip(const std::string& path);

        // Decrement ref count. Resource unloaded when it reaches zero.
        void ReleaseClip(AnimHandle handle);

        bool IsReady(AnimHandle handle) const;

        // Borrowed pointer — valid only for current call stack.
        const AnimationResource* GetResource(AnimHandle handle) const;

        // Mutable borrowed pointer — EDITOR ONLY. Lets the animation-timeline
        // editor author notify tracks directly on the loaded resource and
        // re-serialize it to .ianim. Valid only for the current call stack; do
        // NOT store. Returns nullptr if the handle isn't ready.
        AnimationResource* GetResourceMutable(AnimHandle handle);

        // ---- Per-frame pump -------------------------------------------------

        // Promote ResourceManager-ready clips into the Ready state.
        // Call once per frame after BeginFrame.
        void Tick();

        // Resolve PendingAnimBind components on all entities in the world.
        // Called once per frame after Tick(). When a pending clip becomes ready,
        // binds it to the skeleton, sets AnimationComponent.primaryClip, and
        // removes the PendingAnimBind component.
        void ResolvePendingBinds(::World& world, ::Renderer& renderer);

        // ---- Skeleton binding -----------------------------------------------

        // Bind all clips in the resource to the given skeleton and register
        // them in clipLib.  Returns the ClipLibrary index of the FIRST clip
        // (subsequent clips are contiguous).
        // Returns kInvalidClipIndex if the handle is not yet ready.
        // Safe to call multiple times (subsequent calls return the cached index).
        uint32_t BindToSkeleton(AnimHandle           handle,
                                 const SkeletonAsset& skeleton,
                                 ClipLibrary&         clipLib);

        // Convert morph clips in the resource to MorphClipAssets and register them
        // in morphLib.  Returns the MorphClipLibrary index of the FIRST morph clip,
        // or kInvalidClipIndex if the resource has no morph data or is not ready.
        // Safe to call multiple times (subsequent calls return the cached index).
        uint32_t BindMorphClip(AnimHandle        handle,
                                MorphClipLibrary& morphLib);

        // ---- Lifecycle ------------------------------------------------------

        void Shutdown();

    private:
        static std::string NormalizePath(const std::string& path);

        enum class ClipState { Loading, Ready };

        struct ClipEntry
        {
            AnimHandle   handle;
            Handle       rmHandle;          // ResourceManager handle
            ClipState    state    = ClipState::Loading;
            uint32_t     refCount = 0;

            // Set after BindToSkeleton succeeds (first call only).
            uint32_t     firstClipLibIdx  = kInvalidClipIndex;
            // Set after BindMorphClip succeeds (first call only).
            uint32_t     firstMorphLibIdx = kInvalidClipIndex;

            // Per-skeleton cache: different skeletons produce different bound clips.
            // Key = simple hash of skeleton identity, value = firstClipLibIdx for that skeleton.
            std::unordered_map<uint32_t, uint32_t> perSkelCache;
        };

        ResourceManager* m_rm = nullptr;

        mutable std::mutex m_mutex;

        uint32_t                                        m_nextSlot = 1;
        std::unordered_map<std::string, ClipEntry>      m_byPath;
        std::unordered_map<uint32_t, std::string>       m_pathBySlot; // slot → normalized path
    };
}
