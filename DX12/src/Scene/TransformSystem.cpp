#include "Scene/TransformSystem.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"

#include <cfloat>
#include <vector>

void TransformSystem::Propagate(World& world)
{
    using namespace DirectX;

    // ---- Pass 1: find root entities and seed the BFS queue ----
    // A root entity has a GlobalTransform but either no Parent component,
    // or a Parent whose entity is NullEntity.
    //
    // Persistent vector-as-queue reused across frames. std::queue (backed by
    // std::deque) allocated ~1 chunk per push_back over 64-entity capacity and
    // freed on pop_front — profiling showed >40% of CPU in operator new here.
    // vector + head index keeps capacity across frames → zero steady-state alloc.
    thread_local std::vector<Entity> s_queue;
    s_queue.clear();
    size_t head = 0;

    // Cache pool pointers once per call — World::GetComponent<T> does an
    // unordered_map lookup by type_index on every call; in Propagate's inner
    // loop this accounted for ~14% of total CPU. Pools are long-lived (created
    // at scene load), so one lookup per tick is enough.
    auto* poolGT       = world.EnsurePool<GlobalTransform>();
    auto* poolLT       = world.GetPool<LocalTransform>();
    auto* poolParent   = world.GetPool<Parent>();
    auto* poolChildren = world.GetPool<Children>();
    auto* poolVis      = world.GetPool<VisibilityComponent>();
    auto* poolLAabb    = world.GetPool<LocalAabb>();
    auto* poolWAabb    = world.GetPool<WorldAabb>();

    // Walk the GlobalTransform pool's dense array directly instead of
    // filtering GetEntities() through Has(e). On a 22k-entity scene with
    // ~2k GlobalTransform owners the old path cost 20k wasted iterations
    // just to seed the queue; this variant only visits entities that
    // actually carry the component.
    //
    // Safe to iterate by reference: the poolGT->Add() calls below only hit
    // entities already in the pool (we're walking its own dense list), so
    // Add() takes the overwrite branch and never grows m_dense.
    const auto& gtEntities = poolGT->Entities();
    const size_t gtCount = gtEntities.size();
    for (size_t gi = 0; gi < gtCount; ++gi)
    {
        const Entity e = gtEntities[gi];
        const Parent* p = poolParent ? poolParent->Get(e) : nullptr;
        const bool isRoot = (!p || p->entity == NullEntity);

        if (!isRoot)
            continue;

        // Compute root GlobalTransform from LocalTransform
        if (poolLT)
        {
            if (const LocalTransform* lt = poolLT->Get(e))
            {
                GlobalTransform gt;
                XMStoreFloat4x4(&gt.matrix, lt->ToMatrix());
                poolGT->Add(e, gt);
            }
        }

        // Roots have no ancestor to inherit hidden-state from
        if (poolVis)
        {
            if (VisibilityComponent* v = poolVis->Get(e))
                v->inheritedHidden = false;
        }

        s_queue.push_back(e);
    }

    // ---- Pass 2: BFS — parent before children ----
    while (head < s_queue.size())
    {
        const Entity current = s_queue[head++];

        const GlobalTransform* parentGT  = poolGT->Get(current);
        const VisibilityComponent* parentVis = poolVis ? poolVis->Get(current) : nullptr;
        const Children*        children  = poolChildren ? poolChildren->Get(current) : nullptr;

        if (!children || !parentGT)
            continue;

        const XMMATRIX parentMat    = XMLoadFloat4x4(&parentGT->matrix);
        const bool     parentHidden = parentVis && !parentVis->IsEffectivelyVisible();

        for (Entity child : children->entities)
        {
            if (!world.IsAlive(child))
                continue;

            // child.GlobalTransform = child.LocalTransform * parent.GlobalTransform
            const LocalTransform* lt = poolLT ? poolLT->Get(child) : nullptr;
            if (lt)
            {
                GlobalTransform gt;
                XMStoreFloat4x4(&gt.matrix, lt->ToMatrix() * parentMat);
                poolGT->Add(child, gt);
            }

            // Propagate inherited visibility
            if (poolVis)
            {
                if (VisibilityComponent* v = poolVis->Get(child))
                {
                    const bool ancestorHidden = parentHidden
                        || (parentVis && parentVis->inheritedHidden);
                    v->inheritedHidden = ancestorHidden;
                }
            }

            // Update WorldAabb: transform LocalAabb corners by GlobalTransform.
            // LocalAabb is immutable (set at load time); WorldAabb is recomputed each frame.
            const LocalAabb* localAabb = poolLAabb ? poolLAabb->Get(child) : nullptr;
            WorldAabb*       worldAabb = poolWAabb ? poolWAabb->Get(child) : nullptr;
            if (localAabb && worldAabb)
            {
                const GlobalTransform* childGT = poolGT->Get(child);
                if (childGT)
                {
                    const XMMATRIX M = XMLoadFloat4x4(&childGT->matrix);

                    const float corners[8][3] =
                    {
                        {localAabb->min.x, localAabb->min.y, localAabb->min.z},
                        {localAabb->max.x, localAabb->min.y, localAabb->min.z},
                        {localAabb->min.x, localAabb->max.y, localAabb->min.z},
                        {localAabb->max.x, localAabb->max.y, localAabb->min.z},
                        {localAabb->min.x, localAabb->min.y, localAabb->max.z},
                        {localAabb->max.x, localAabb->min.y, localAabb->max.z},
                        {localAabb->min.x, localAabb->max.y, localAabb->max.z},
                        {localAabb->max.x, localAabb->max.y, localAabb->max.z},
                    };

                    XMVECTOR vmin = XMVectorSet(FLT_MAX,  FLT_MAX,  FLT_MAX,  0.f);
                    XMVECTOR vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0.f);

                    for (const auto& c : corners)
                    {
                        XMVECTOR v = XMVector3TransformCoord(
                            XMVectorSet(c[0], c[1], c[2], 1.f), M);
                        vmin = XMVectorMin(vmin, v);
                        vmax = XMVectorMax(vmax, v);
                    }

                    XMStoreFloat3(&worldAabb->min, vmin);
                    XMStoreFloat3(&worldAabb->max, vmax);
                }
            }

            s_queue.push_back(child);
        }
    }
}

void TransformSystem::PropagateSubtree(World& world, unsigned int rootU)
{
    using namespace DirectX;
    const Entity root = static_cast<Entity>(rootU);

    auto* poolGT       = world.GetPool<GlobalTransform>();
    auto* poolChildren = world.GetPool<Children>();
    if (!poolGT || !poolChildren)
        return;

    // Nothing to do unless the root actually has descendants. The common case
    // (a dynamic prop with no children) early-outs here after two lookups.
    const Children* rootChildren = poolChildren->Get(root);
    if (!rootChildren || rootChildren->entities.empty())
        return;
    if (!poolGT->Get(root))   // root must already have a current GlobalTransform
        return;

    auto* poolLT    = world.GetPool<LocalTransform>();
    auto* poolVis   = world.GetPool<VisibilityComponent>();
    auto* poolLAabb = world.GetPool<LocalAabb>();
    auto* poolWAabb = world.GetPool<WorldAabb>();

    // BFS over descendants only — root's GlobalTransform is taken as authoritative
    // (it was just overwritten by the caller). Mirrors Pass 2 of Propagate().
    thread_local std::vector<Entity> s_subQueue;
    s_subQueue.clear();
    size_t head = 0;
    s_subQueue.push_back(root);

    while (head < s_subQueue.size())
    {
        const Entity current = s_subQueue[head++];

        const GlobalTransform*     parentGT  = poolGT->Get(current);
        const VisibilityComponent* parentVis = poolVis ? poolVis->Get(current) : nullptr;
        const Children*            children  = poolChildren->Get(current);

        if (!children || !parentGT)
            continue;

        const XMMATRIX parentMat    = XMLoadFloat4x4(&parentGT->matrix);
        const bool     parentHidden = parentVis && !parentVis->IsEffectivelyVisible();

        for (Entity child : children->entities)
        {
            if (!world.IsAlive(child))
                continue;

            const LocalTransform* lt = poolLT ? poolLT->Get(child) : nullptr;
            if (lt)
            {
                GlobalTransform gt;
                XMStoreFloat4x4(&gt.matrix, lt->ToMatrix() * parentMat);
                poolGT->Add(child, gt);
            }

            if (poolVis)
            {
                if (VisibilityComponent* v = poolVis->Get(child))
                {
                    const bool ancestorHidden = parentHidden
                        || (parentVis && parentVis->inheritedHidden);
                    v->inheritedHidden = ancestorHidden;
                }
            }

            const LocalAabb* localAabb = poolLAabb ? poolLAabb->Get(child) : nullptr;
            WorldAabb*       worldAabb = poolWAabb ? poolWAabb->Get(child) : nullptr;
            if (localAabb && worldAabb)
            {
                const GlobalTransform* childGT = poolGT->Get(child);
                if (childGT)
                {
                    const XMMATRIX M = XMLoadFloat4x4(&childGT->matrix);
                    const float corners[8][3] =
                    {
                        {localAabb->min.x, localAabb->min.y, localAabb->min.z},
                        {localAabb->max.x, localAabb->min.y, localAabb->min.z},
                        {localAabb->min.x, localAabb->max.y, localAabb->min.z},
                        {localAabb->max.x, localAabb->max.y, localAabb->min.z},
                        {localAabb->min.x, localAabb->min.y, localAabb->max.z},
                        {localAabb->max.x, localAabb->min.y, localAabb->max.z},
                        {localAabb->min.x, localAabb->max.y, localAabb->max.z},
                        {localAabb->max.x, localAabb->max.y, localAabb->max.z},
                    };
                    XMVECTOR vmin = XMVectorSet(FLT_MAX,  FLT_MAX,  FLT_MAX,  0.f);
                    XMVECTOR vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0.f);
                    for (const auto& c : corners)
                    {
                        XMVECTOR v = XMVector3TransformCoord(
                            XMVectorSet(c[0], c[1], c[2], 1.f), M);
                        vmin = XMVectorMin(vmin, v);
                        vmax = XMVectorMax(vmax, v);
                    }
                    XMStoreFloat3(&worldAabb->min, vmin);
                    XMStoreFloat3(&worldAabb->max, vmax);
                }
            }

            s_subQueue.push_back(child);
        }
    }
}
