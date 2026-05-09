#pragma once

// Simple ECS with sparse-set component storage.
//
// Design:
//   - Entity is an opaque uint32_t ID (0 = null sentinel).
//   - World owns one ComponentPool<T> per component type (lazily created).
//   - ComponentPool<T> stores values in a dense std::vector<T> keyed by a
//     direct sparse[Entity] → dense-index lookup.  GetComponent<T>(e) is
//     two array loads — no hash probes, no std::any boxing, no any_cast.
//   - ForEach<T>(fn) iterates the dense data cache-friendly and visits
//     only entities that actually have T.
//
// Rewrote from the original unordered_map-of-std::any storage in the P1-P6
// follow-up to cut Bistro's 22k-entity BuildRenderScene / TransformSystem
// cost: the original needed 2 hash lookups + an any_cast per GetComponent,
// which at ~10 GetComponent calls per entity per frame dominated the CPU
// frame on large scenes.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

// Entity: opaque ID
using Entity = std::uint32_t;

constexpr Entity NullEntity = 0u;

// EntityHandle — Entity + generation for lifetime-safe references.
//
// Raw Entity IDs are recycled by CreateEntity's free list, so a component
// field holding a bare Entity can silently start pointing at a brand-new
// entity that happens to reuse the slot. Components that need to survive
// the target's destruction (FollowEntityComponent / FollowSocketComponent
// target, event payload) should store EntityHandle instead and gate reads
// on World::IsHandleValid(h). Existing Entity fields that are cleared
// proactively on destroy are fine to leave alone.
struct EntityHandle
{
    Entity   entity     = NullEntity;
    uint32_t generation = 0u;

    bool operator==(const EntityHandle& o) const
    { return entity == o.entity && generation == o.generation; }
    bool operator!=(const EntityHandle& o) const { return !(*this == o); }
};

constexpr EntityHandle NullEntityHandle{ NullEntity, 0u };

// Component base (tag only; concrete data lives in each component type)
struct ComponentBase {};

// ---------------------------------------------------------------------------
// ComponentPool — sparse-set storage for one component type.
// ---------------------------------------------------------------------------
class IComponentPool
{
public:
    virtual ~IComponentPool()                          = default;
    virtual void Erase(Entity e)                        = 0;
    virtual bool Has(Entity e) const                    = 0;
    virtual void Clear()                                = 0;
};

template <typename T>
class ComponentPool : public IComponentPool
{
public:
    // Add or overwrite the value for entity e. Returns a pointer to the
    // (new or existing) slot so callers can use the return value.
    T* Add(Entity e, T value)
    {
        EnsureSparse(e);
        const int32_t existing = m_sparse[e];
        if (existing >= 0)
        {
            m_data[existing] = std::move(value);
            return &m_data[existing];
        }
        const int32_t idx = static_cast<int32_t>(m_data.size());
        m_sparse[e] = idx;
        m_dense.push_back(e);
        m_data.push_back(std::move(value));
        return &m_data.back();
    }

    T* Get(Entity e)
    {
        if (e >= m_sparse.size()) return nullptr;
        const int32_t idx = m_sparse[e];
        if (idx < 0) return nullptr;
        return &m_data[idx];
    }

    const T* Get(Entity e) const
    {
        if (e >= m_sparse.size()) return nullptr;
        const int32_t idx = m_sparse[e];
        if (idx < 0) return nullptr;
        return &m_data[idx];
    }

    void Erase(Entity e) override
    {
        if (e >= m_sparse.size()) return;
        const int32_t idx = m_sparse[e];
        if (idx < 0) return;

        const int32_t lastIdx = static_cast<int32_t>(m_data.size()) - 1;
        if (idx != lastIdx)
        {
            // Swap-with-back to keep dense contiguous; patch the sparse
            // mapping of the entity we moved into this slot.
            m_data[idx]  = std::move(m_data[lastIdx]);
            m_dense[idx] = m_dense[lastIdx];
            m_sparse[m_dense[idx]] = idx;
        }
        m_data.pop_back();
        m_dense.pop_back();
        m_sparse[e] = -1;
    }

    bool Has(Entity e) const override
    {
        return e < m_sparse.size() && m_sparse[e] >= 0;
    }

    void Clear() override
    {
        m_sparse.clear();
        m_dense.clear();
        m_data.clear();
    }

    // Iteration helpers — dense vectors are cache-friendly. Indices line
    // up: m_dense[i] is the owner of m_data[i].
    size_t                       Size()     const { return m_data.size(); }
    const std::vector<Entity>&   Entities() const { return m_dense; }
    std::vector<T>&              Data()           { return m_data; }
    const std::vector<T>&        Data()     const { return m_data; }

private:
    void EnsureSparse(Entity e)
    {
        if (e >= m_sparse.size())
            m_sparse.resize(static_cast<size_t>(e) + 1, -1);
    }

    std::vector<int32_t> m_sparse; // Entity → index in m_data (or -1)
    std::vector<Entity>  m_dense;  // m_dense[i] owns m_data[i]
    std::vector<T>       m_data;
};

// ---------------------------------------------------------------------------
// World: manages entities, names, and sparse-set component storage.
// ---------------------------------------------------------------------------
class World
{
public:
    World() = default;

    // ---- Entity lifecycle --------------------------------------------------

    Entity CreateEntity()
    {
        Entity e;
        if (m_freeList.empty())
        {
            e = m_nextEntity++;
        }
        else
        {
            e = m_freeList.back();
            m_freeList.pop_back();
        }
        m_entityIndex[e] = m_entities.size();
        m_entities.push_back(e);
        if (e >= m_alive.size()) m_alive.resize(static_cast<size_t>(e) + 1, false);
        if (e >= m_generations.size()) m_generations.resize(static_cast<size_t>(e) + 1, 0u);
        m_alive[e] = true;
        // Generation starts at 1 on first creation, bumps on every destroy so
        // a stale EntityHandle can never match a recycled slot's value.
        if (m_generations[e] == 0u) m_generations[e] = 1u;
        return e;
    }

    // O(1): swap-with-back into m_entities instead of std::find + erase.
    // On a 22k-entity scene the old std::find + vector::erase meant tens of
    // thousands of ops per destroy; spawning/destroying projectiles or
    // particles in play would stall BuildRenderScene. The swap-with-back
    // reorders m_entities, but no caller relies on its order.
    void DestroyEntity(Entity e)
    {
        // Fire entity-destroy callbacks BEFORE we tear the entity down so
        // listeners can still query components on `e` one last time (e.g.
        // MaterialComponent -> release texture handles by path). Copy the
        // listener list so a callback adding/removing listeners mid-fire
        // can't invalidate our iterator.
        if (!m_destroyListeners.empty())
        {
            const auto snapshot = m_destroyListeners;
            for (const auto& L : snapshot) L.fn(e);
        }

        for (auto& [typeIdx, pool] : m_pools)
            pool->Erase(e);
        m_names.erase(e);

        auto idxIt = m_entityIndex.find(e);
        if (idxIt != m_entityIndex.end())
        {
            const size_t idx  = idxIt->second;
            const size_t last = m_entities.size() - 1;
            if (idx != last)
            {
                const Entity moved = m_entities[last];
                m_entities[idx]     = moved;
                m_entityIndex[moved] = idx;
            }
            m_entities.pop_back();
            m_entityIndex.erase(idxIt);
        }
        m_freeList.push_back(e);
        if (e < m_alive.size()) m_alive[e] = false;
        if (e < m_generations.size()) ++m_generations[e];
    }

    // ---- Entity-destroy callback registry ----------------------------------
    // Systems that maintain per-entity caches (Renderer texture cache,
    // SkinnedMesh prev-pose cache, physics chain state, script state, …)
    // register a callback here. The callback fires from DestroyEntity BEFORE
    // the entity's components are erased, so the listener can still read any
    // component it needs to finalize cleanup (e.g. path-based texture refcount
    // release). IDs are recycled by CreateEntity's free list — without these
    // callbacks a freshly-created entity that reuses a destroyed ID inherits
    // every stale cache entry keyed on that ID.
    using EntityDestroyFn = std::function<void(Entity)>;
    using EntityDestroyListenerHandle = uint32_t;

    [[nodiscard]] EntityDestroyListenerHandle
    AddEntityDestroyListener(EntityDestroyFn fn)
    {
        const EntityDestroyListenerHandle id = m_nextListenerId++;
        m_destroyListeners.push_back({ id, std::move(fn) });
        return id;
    }

    void RemoveEntityDestroyListener(EntityDestroyListenerHandle h)
    {
        if (h == 0) return;
        auto it = std::find_if(m_destroyListeners.begin(), m_destroyListeners.end(),
            [h](const DestroyListener& L) { return L.id == h; });
        if (it != m_destroyListeners.end()) m_destroyListeners.erase(it);
    }

    void Clear()
    {
        m_pools.clear();
        m_names.clear();
        m_freeList.clear();
        m_entities.clear();
        m_entityIndex.clear();
        m_alive.clear();
        m_generations.clear();
        m_nextEntity = 1;
        // Listeners survive Clear(): systems subscribe once per scene / engine
        // lifetime, and the OnWorldClear path (Renderer::OnWorldClear etc.)
        // already wipes their caches explicitly. Clearing listeners here would
        // silently unhook every system and leak stale caches on the next
        // scene's entity destroys.
    }

    bool IsAlive(Entity e) const
    {
        return e < m_alive.size() && m_alive[e];
    }

    // Current generation of e (0 if never existed). Used to stamp handles.
    uint32_t GetGeneration(Entity e) const
    {
        return e < m_generations.size() ? m_generations[e] : 0u;
    }

    // Stamp a handle from a live entity. Returns NullEntityHandle if dead.
    EntityHandle MakeHandle(Entity e) const
    {
        if (!IsAlive(e)) return NullEntityHandle;
        return EntityHandle{ e, m_generations[e] };
    }

    // A handle is valid iff its entity is still alive AND its generation
    // matches — catches the "recycled slot" case that plain IsAlive misses.
    bool IsHandleValid(EntityHandle h) const
    {
        if (h.entity == NullEntity) return false;
        if (!IsAlive(h.entity))     return false;
        return m_generations[h.entity] == h.generation;
    }

    const std::vector<Entity>& GetEntities() const { return m_entities; }

    // ---- Entity naming -----------------------------------------------------

    void SetName(Entity e, std::string name) { m_names[e] = std::move(name); }

    const std::string& GetName(Entity e) const
    {
        static const std::string kUnnamed = "(unnamed)";
        auto it = m_names.find(e);
        return it != m_names.end() ? it->second : kUnnamed;
    }

    // ---- Component storage -------------------------------------------------

    template <typename T>
    void AddComponent(Entity e, T component)
    {
        GetOrCreatePool<T>()->Add(e, std::move(component));
    }

    template <typename T>
    T* GetComponent(Entity e)
    {
        auto* p = GetPool<T>();
        return p ? p->Get(e) : nullptr;
    }

    template <typename T>
    const T* GetComponent(Entity e) const
    {
        auto* p = GetPool<T>();
        return p ? p->Get(e) : nullptr;
    }

    template <typename T>
    bool HasComponent(Entity e) const
    {
        const auto* p = GetPool<T>();
        return p && p->Has(e);
    }

    template <typename T>
    void RemoveComponent(Entity e)
    {
        auto* p = GetPool<T>();
        if (p) p->Erase(e);
    }

    // Dense iteration — visits only entities that have T. For a system that
    // reads/writes one component this is strictly faster than walking
    // GetEntities() and calling GetComponent<T>() per entity, because the
    // loop touches only the T pool's dense array (cache-friendly) and skips
    // entities that don't have T.
    template <typename T, typename Fn>
    void ForEach(Fn&& fn)
    {
        auto* p = GetPool<T>();
        if (!p) return;
        auto& ents = p->Entities();
        auto& data = p->Data();
        const size_t n = data.size();
        for (size_t i = 0; i < n; ++i)
            fn(ents[i], data[i]);
    }

    template <typename T, typename Fn>
    void ForEach(Fn&& fn) const
    {
        const auto* p = GetPool<T>();
        if (!p) return;
        const auto& ents = p->Entities();
        const auto& data = p->Data();
        const size_t n = data.size();
        for (size_t i = 0; i < n; ++i)
            fn(ents[i], data[i]);
    }

    // Like GetPool<T>() but creates the pool if it does not yet exist.
    // Systems that write a component every frame should cache this pointer
    // once per tick to avoid the m_pools.find() hash lookup on every access.
    template <typename T>
    ComponentPool<T>* EnsurePool() { return GetOrCreatePool<T>(); }

    // Public pool access — used by GetComponentTypeIndices and the editor's
    // component-tag registry (each pool is exposed via its type_index key).
    template <typename T>
    ComponentPool<T>* GetPool()
    {
        const auto it = m_pools.find(std::type_index(typeid(T)));
        return it == m_pools.end()
            ? nullptr
            : static_cast<ComponentPool<T>*>(it->second.get());
    }

    template <typename T>
    const ComponentPool<T>* GetPool() const
    {
        const auto it = m_pools.find(std::type_index(typeid(T)));
        return it == m_pools.end()
            ? nullptr
            : static_cast<const ComponentPool<T>*>(it->second.get());
    }

    // Returns the std::type_index of every component type currently attached
    // to entity e. Order is unspecified (reflects internal hash-map order).
    std::vector<std::type_index> GetComponentTypeIndices(Entity e) const
    {
        std::vector<std::type_index> out;
        out.reserve(m_pools.size());
        for (const auto& [typeIdx, pool] : m_pools)
            if (pool->Has(e))
                out.push_back(typeIdx);
        return out;
    }

private:
    template <typename T>
    ComponentPool<T>* GetOrCreatePool()
    {
        const auto ti = std::type_index(typeid(T));
        const auto it = m_pools.find(ti);
        if (it != m_pools.end())
            return static_cast<ComponentPool<T>*>(it->second.get());
        auto pool = std::make_unique<ComponentPool<T>>();
        ComponentPool<T>* raw = pool.get();
        m_pools.emplace(ti, std::move(pool));
        return raw;
    }

    std::vector<Entity>                    m_entities;
    // Entity → index in m_entities. Enables O(1) DestroyEntity.
    std::unordered_map<Entity, size_t>     m_entityIndex;
    std::vector<Entity>                    m_freeList;
    std::vector<bool>                      m_alive;
    // Parallel to m_alive: bumped on DestroyEntity so stale EntityHandles
    // fail IsHandleValid even after the slot is reused.
    std::vector<uint32_t>                  m_generations;
    Entity                                 m_nextEntity{ 1u };
    std::unordered_map<Entity, std::string> m_names;

    // One pool per component type. Owned by World; type-erased through
    // IComponentPool so DestroyEntity / Clear can iterate uniformly.
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_pools;

    struct DestroyListener
    {
        EntityDestroyListenerHandle id;
        EntityDestroyFn             fn;
    };
    std::vector<DestroyListener> m_destroyListeners;
    EntityDestroyListenerHandle  m_nextListenerId{ 1u };
};

// System base: override Tick() to render/update using CommandList / Graphics
struct SystemBase
{
    virtual ~SystemBase() = default;
    virtual void Tick(float /*deltaTime*/) {}
};
