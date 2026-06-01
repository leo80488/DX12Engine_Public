#include "Resource/AnimationClipSystem.h"
#include "ECS/AnimationComponents.h"
#include "Graphics/Renderer.h"
#include "System/Log.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <string>

namespace Resource
{
    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static std::string NormalizePathImpl(const std::string& path)
    {
        namespace fs = std::filesystem;
        std::string norm = fs::path(path).lexically_normal().string();
        for (char& c : norm) if (c == '\\') c = '/';
        for (char& c : norm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return norm;
    }

    // =========================================================================
    // AnimationClipSystem
    // =========================================================================
    void AnimationClipSystem::Init(ResourceManager& rm)
    {
        m_rm = &rm;
    }

    AnimHandle AnimationClipSystem::AcquireClip(const std::string& path)
    {
        assert(m_rm && "AnimationClipSystem::Init not called");
        const std::string norm = NormalizePathImpl(path);

        std::lock_guard<std::mutex> lk(m_mutex);

        auto it = m_byPath.find(norm);
        if (it != m_byPath.end())
        {
            ++it->second.refCount;
            return it->second.handle;
        }

        // New entry — allocate a slot and start async load
        const uint32_t slot = m_nextSlot++;
        Handle rmHandle = m_rm->Load(path, ResourceType::Animation);

        AnimHandle ah = Handle::Make(slot, ResourceType::Animation, 1u);

        ClipEntry entry{};
        entry.handle   = ah;
        entry.rmHandle = rmHandle;
        entry.refCount = 1;
        entry.state    = ClipState::Loading;

        m_byPath[norm]    = entry;
        m_pathBySlot[slot] = norm;

        LOG_INFO("AnimationClipSystem: AcquireClip '%s' slot=%u", path.c_str(), slot);
        return ah;
    }

    void AnimationClipSystem::ReleaseClip(AnimHandle handle)
    {
        if (handle == kInvalidAnimHandle2) return;

        std::lock_guard<std::mutex> lk(m_mutex);

        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return;

        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end()) return;

        ClipEntry& entry = entryIt->second;
        if (--entry.refCount == 0)
        {
            if (m_rm) m_rm->Unload(entry.rmHandle);
            m_byPath.erase(entryIt);
            m_pathBySlot.erase(pathIt);
            LOG_INFO("AnimationClipSystem: released slot=%u", slot);
        }
    }

    bool AnimationClipSystem::IsReady(AnimHandle handle) const
    {
        if (handle == kInvalidAnimHandle2) return false;
        std::lock_guard<std::mutex> lk(m_mutex);
        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return false;
        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end()) return false;
        return entryIt->second.state == ClipState::Ready;
    }

    const AnimationResource* AnimationClipSystem::GetResource(AnimHandle handle) const
    {
        if (handle == kInvalidAnimHandle2) return nullptr;
        std::lock_guard<std::mutex> lk(m_mutex);
        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return nullptr;
        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end() || entryIt->second.state != ClipState::Ready)
            return nullptr;
        return m_rm->Get<AnimationResource>(entryIt->second.rmHandle);
    }

    AnimationResource* AnimationClipSystem::GetResourceMutable(AnimHandle handle)
    {
        // The resource is owned (non-const) by ResourceManager; GetResource
        // only hands out const for safety. Editor authoring is an explicit
        // opt-in to mutate the asset in memory before re-serializing.
        return const_cast<AnimationResource*>(GetResource(handle));
    }

    void AnimationClipSystem::Tick()
    {
        if (!m_rm) return;
        std::lock_guard<std::mutex> lk(m_mutex);

        for (auto& [path, entry] : m_byPath)
        {
            if (entry.state != ClipState::Loading) continue;
            if (m_rm->GetState(entry.rmHandle) == ResourceState::Ready)
            {
                entry.state = ClipState::Ready;
                LOG_SUCCESS("AnimationClipSystem: '%s' ready", path.c_str());
            }
            else if (m_rm->GetState(entry.rmHandle) == ResourceState::Failed)
            {
                LOG_ERROR("AnimationClipSystem: load failed for '%s'", path.c_str());
                // Leave in Loading so repeated Tick calls don't spam the log.
                entry.state = ClipState::Ready; // mark ready to stop polling; resource is null
            }
        }
    }

    void AnimationClipSystem::ResolvePendingBinds(::World& world, ::Renderer& renderer)
    {
        std::vector<Entity> resolved;

        for (Entity e : world.GetEntities())
        {
            auto* pending = world.GetComponent<PendingAnimBind>(e);
            if (!pending) continue;

            // Reconstruct AnimHandle from stored packed value
            AnimHandle ah;
            ah.packed = pending->handlePacked;
            if (!IsReady(ah)) continue; // still loading

            auto* skelComp = world.GetComponent<SkeletonComponent>(e);
            if (!skelComp || skelComp->assetIndex == kInvalidAnimHandle)
            { resolved.push_back(e); continue; }

            const SkeletonAsset& skel = renderer.GetSkeletonRegistry().Get(skelComp->assetIndex);
            uint32_t clipIdx = BindToSkeleton(ah, skel, renderer.GetClipLibrary());
            if (clipIdx != kInvalidClipIndex)
            {
                auto* animComp = world.GetComponent<AnimationComponent>(e);
                if (animComp) animComp->primaryClip = clipIdx;
            }

            uint32_t morphIdx = BindMorphClip(ah, renderer.GetMorphClipLibrary());
            if (morphIdx != kInvalidClipIndex)
            {
                auto* morphComp = world.GetComponent<MorphComponent>(e);
                if (!morphComp)
                { MorphComponent mc{}; mc.primaryMorphClip = morphIdx; world.AddComponent(e, mc); }
                else morphComp->primaryMorphClip = morphIdx;
            }

            LOG_SUCCESS("AnimationClipSystem: deferred bind resolved for entity %u ('%s')",
                        e, pending->animPath.c_str());
            resolved.push_back(e);
        }

        for (Entity e : resolved)
            world.RemoveComponent<PendingAnimBind>(e);
    }

    uint32_t AnimationClipSystem::BindToSkeleton(AnimHandle           handle,
                                                   const SkeletonAsset& skeleton,
                                                   ClipLibrary&         clipLib)
    {
        if (!IsReady(handle)) return kInvalidClipIndex;

        std::lock_guard<std::mutex> lk(m_mutex);

        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return kInvalidClipIndex;
        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end()) return kInvalidClipIndex;

        ClipEntry& entry = entryIt->second;

        // Per-skeleton cache: different skeletons need different bound clips
        // because bone ordering differs between models.
        const uint32_t skelBoneCount = skeleton.boneCount;
        // Use bone count + first bone name hash as a simple skeleton identity key.
        uint32_t skelKey = skelBoneCount;
        if (skelBoneCount > 0)
            skelKey ^= skeleton.nameToIndex.size() * 2654435761u; // mix in hash count

        auto cacheIt = entry.perSkelCache.find(skelKey);
        if (cacheIt != entry.perSkelCache.end())
            return cacheIt->second;

        const AnimationResource* res = m_rm->Get<AnimationResource>(entry.rmHandle);
        if (!res || res->clips.empty()) return kInvalidClipIndex;

        const uint32_t firstIdx = clipLib.Count();
        for (const AnimClipData& acd : res->clips)
        {
            ClipAsset bound = acd.BindToSkeleton(skeleton);
            clipLib.Register(std::move(bound));
            LOG_INFO("AnimationClipSystem: bound clip '%s' → ClipLibrary[%u] (skelBones=%u)",
                     acd.name, clipLib.Count() - 1, skelBoneCount);
        }

        entry.perSkelCache[skelKey] = firstIdx;
        if (entry.firstClipLibIdx == kInvalidClipIndex)
            entry.firstClipLibIdx = firstIdx;
        return firstIdx;
    }

    uint32_t AnimationClipSystem::BindMorphClip(AnimHandle        handle,
                                                  MorphClipLibrary& morphLib)
    {
        if (!IsReady(handle)) return kInvalidClipIndex;

        std::lock_guard<std::mutex> lk(m_mutex);

        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return kInvalidClipIndex;
        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end()) return kInvalidClipIndex;

        ClipEntry& entry = entryIt->second;

        // Already bound — return cached first index
        if (entry.firstMorphLibIdx != kInvalidClipIndex)
            return entry.firstMorphLibIdx;

        const AnimationResource* res = m_rm->Get<AnimationResource>(entry.rmHandle);
        if (!res || res->morphClips.empty()) return kInvalidClipIndex;

        const uint32_t firstIdx = morphLib.Count();
        for (const MorphClipData& mcd : res->morphClips)
        {
            MorphClipAsset asset = mcd.ToRuntimeClip();
            morphLib.Register(std::move(asset));
            LOG_INFO("AnimationClipSystem: bound morph clip '%s' → MorphClipLibrary[%u]",
                     mcd.name, morphLib.Count() - 1);
        }

        entry.firstMorphLibIdx = firstIdx;
        return firstIdx;
    }

    void AnimationClipSystem::Shutdown()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_rm)
        {
            for (auto& [path, entry] : m_byPath)
                m_rm->Unload(entry.rmHandle);
        }
        m_byPath.clear();
        m_pathBySlot.clear();
    }

    std::string AnimationClipSystem::NormalizePath(const std::string& path)
    {
        return NormalizePathImpl(path);
    }

} // namespace Resource
