#pragma once

// PostProcess::ProfileSystem — vends generational handles to shared
// PostProcessProfile resources, mirroring TextureSystem's path-dedup +
// refcount model. A singleton (like AssetFS / GuidRegistry) so the ECS resolve
// system, editor, scene serializer and Lua bindings can all reach profiles
// without plumbing a pointer through every layer.
//
// Profiles are CPU-side and small, so loading is synchronous (straight through
// AssetFS) rather than via the async ResourceManager — simpler and race-free.
//
// The engine-default profile (all properties overriding the shipping values)
// lives here too; it is the base layer of every resolve and what the editor's
// global look inspector edits.

#include "PostProcess/PostProcessProfile.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace PostProcess
{

class ProfileSystem
{
public:
    static ProfileSystem& Get();

    ProfileSystem(const ProfileSystem&)            = delete;
    ProfileSystem& operator=(const ProfileSystem&) = delete;

    // ---- Engine default look (base layer of every resolve) ----------------
    PostProcessProfile&       EngineDefault()       { return m_engineDefault; }
    const PostProcessProfile& EngineDefault() const { return m_engineDefault; }
    void ResetEngineDefault() { m_engineDefault = MakeEngineDefaultProfile(); }

    // ---- Asset-backed profiles --------------------------------------------
    // Load (or return the cached) .ppprofile at @p path. Path-deduped and
    // refcounted. Returns an invalid handle if the file can't be loaded.
    ProfileHandle Acquire(const std::string& path);

    // In-memory profile with no backing file (Lua inline overrides, editor
    // "new profile"). Returns a valid handle; not path-deduped.
    ProfileHandle CreateRuntime(const PostProcessProfile& profile = PostProcessProfile{});

    void Release(ProfileHandle h);

    PostProcessProfile*       Get(ProfileHandle h);
    const PostProcessProfile* Get(ProfileHandle h) const;

    // Backing path of @p h (empty string for runtime profiles / invalid h).
    const std::string& GetPath(ProfileHandle h) const;

    // Serialize the profile behind @p h to @p path. If the handle had no
    // backing path yet, it adopts @p path (so it dedups on the next Acquire).
    bool Save(ProfileHandle h, const std::string& path);

    // Drop every asset/runtime profile (engine default is kept). Use on world
    // clear to avoid leaking profiles across scene loads.
    void Clear();

private:
    ProfileSystem() { m_engineDefault = MakeEngineDefaultProfile(); }

    struct Slot
    {
        bool               occupied   = false;
        uint32_t           generation = 1;   // bumped on free; never 0
        uint32_t           refCount   = 0;
        std::string        path;             // empty for runtime profiles
        PostProcessProfile profile;
    };

    uint32_t AllocSlot();
    void     FreeSlot(uint32_t idx);
    Slot*    Resolve(ProfileHandle h);
    const Slot* Resolve(ProfileHandle h) const;
    static uint64_t HashPath(const std::string& p);

    PostProcessProfile                     m_engineDefault;
    std::vector<Slot>                      m_slots;
    std::vector<uint32_t>                  m_freeList;
    std::unordered_map<uint64_t, uint32_t> m_pathToSlot;
    std::string                            m_emptyPath;
};

} // namespace PostProcess
