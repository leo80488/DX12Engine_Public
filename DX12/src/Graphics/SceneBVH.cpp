#include "Graphics/SceneBVH.h"
#include "System/TaskSystem.h"
#include <algorithm>
#include <cstring>
#include <atomic>

static float GetAxis(const DirectX::XMFLOAT3& v, int axis)
{
    return (axis == 0) ? v.x : (axis == 1) ? v.y : v.z;
}

// ---------------------------------------------------------------------------
// Build — full SAH rebuild
// ---------------------------------------------------------------------------
void SceneBVH::Build(const Entity* entities, const AABB* aabbs, const LeafType* types, uint32_t count)
{
    m_nodes.clear();
    m_leafEntities.resize(count);
    m_leafAABBs.resize(count);
    m_leafTypes.resize(count);

    if (count == 0) { m_entityLeafSparse.clear(); m_needsRebuild = false; return; }

    std::memcpy(m_leafEntities.data(), entities, count * sizeof(Entity));
    std::memcpy(m_leafAABBs.data(), aabbs, count * sizeof(AABB));
    if (types)
        std::memcpy(m_leafTypes.data(), types, count * sizeof(LeafType));
    else
        std::memset(m_leafTypes.data(), 0, count * sizeof(LeafType)); // all Static

    m_nodes.reserve(count * 2);

    BuildRecursive(0, count, ~0u);

    // Build entity → leaf index sparse array. Size it to max(entity)+1.
    Entity maxEntity = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (m_leafEntities[i] > maxEntity) maxEntity = m_leafEntities[i];
    m_entityLeafSparse.assign(static_cast<size_t>(maxEntity) + 1, kInvalidLeafIdx);
    for (uint32_t i = 0; i < count; ++i)
        m_entityLeafSparse[m_leafEntities[i]] = i;

    m_needsRebuild = false;
}

// ---------------------------------------------------------------------------
uint32_t SceneBVH::BuildRecursive(uint32_t begin, uint32_t end, uint32_t parentIdx)
{
    uint32_t nodeIdx = static_cast<uint32_t>(m_nodes.size());
    m_nodes.push_back(Node{});
    Node& node = m_nodes[nodeIdx];
    node.parent = parentIdx;

    AABB bounds;
    for (uint32_t i = begin; i < end; ++i)
        bounds.Expand(m_leafAABBs[i]);
    node.bounds = bounds;

    uint32_t count = end - begin;
    if (count <= 4)
    {
        node.first = begin;
        node.count = count;
        return nodeIdx;
    }

    // SAH 8-bin split
    float bestCost  = 1e30f;
    int   bestAxis  = 0;
    float bestSplit = 0;

    for (int axis = 0; axis < 3; ++axis)
    {
        float axisMin = GetAxis(bounds.min, axis);
        float axisMax = GetAxis(bounds.max, axis);
        if (axisMax - axisMin < 1e-6f) continue;

        constexpr int kBins = 8;
        struct Bin { AABB bounds; uint32_t count = 0; } bins[kBins];

        float scale = static_cast<float>(kBins) / (axisMax - axisMin);
        for (uint32_t i = begin; i < end; ++i)
        {
            float c = GetAxis(m_leafAABBs[i].Center(), axis);
            int   b = (std::min)(static_cast<int>((c - axisMin) * scale), kBins - 1);
            bins[b].count++;
            bins[b].bounds.Expand(m_leafAABBs[i]);
        }

        AABB leftBounds;
        uint32_t leftCount = 0;
        float leftAreas[kBins - 1];
        uint32_t leftCounts[kBins - 1];

        for (int i = 0; i < kBins - 1; ++i)
        {
            leftBounds.Expand(bins[i].bounds);
            leftCount += bins[i].count;
            leftAreas[i]  = leftBounds.SurfaceArea();
            leftCounts[i] = leftCount;
        }

        AABB rightBounds;
        uint32_t rightCount = 0;

        for (int i = kBins - 1; i > 0; --i)
        {
            rightBounds.Expand(bins[i].bounds);
            rightCount += bins[i].count;
            float cost = leftCounts[i-1] * leftAreas[i-1] + rightCount * rightBounds.SurfaceArea();
            if (cost < bestCost)
            {
                bestCost  = cost;
                bestAxis  = axis;
                bestSplit = axisMin + static_cast<float>(i) / scale;
            }
        }
    }

    // Partition
    uint32_t splitIdx = begin;
    for (uint32_t i = begin; i < end; ++i)
    {
        float c = GetAxis(m_leafAABBs[i].Center(), bestAxis);
        if (c < bestSplit)
        {
            if (i != splitIdx)
            {
                std::swap(m_leafEntities[i], m_leafEntities[splitIdx]);
                std::swap(m_leafAABBs[i], m_leafAABBs[splitIdx]);
                std::swap(m_leafTypes[i], m_leafTypes[splitIdx]);
            }
            splitIdx++;
        }
    }

    if (splitIdx == begin || splitIdx == end)
        splitIdx = begin + count / 2;

    node.left  = BuildRecursive(begin, splitIdx, nodeIdx);
    m_nodes[nodeIdx].right = BuildRecursive(splitIdx, end, nodeIdx);
    m_nodes[nodeIdx].count = 0;

    return nodeIdx;
}

// ---------------------------------------------------------------------------
// RefitLeaf — update one leaf's AABB by entity ID
// ---------------------------------------------------------------------------
void SceneBVH::RefitLeaf(Entity entity, const AABB& newAabb)
{
    // Sparse-array lookup — two array loads, no hashing.
    if (entity >= m_entityLeafSparse.size()) return;
    const uint32_t leafIdx = m_entityLeafSparse[entity];
    if (leafIdx == kInvalidLeafIdx) return;
    m_leafAABBs[leafIdx] = newAabb;
}

// ---------------------------------------------------------------------------
// RefitInternal — bottom-up refit all internal node bounds
// ---------------------------------------------------------------------------
void SceneBVH::RefitInternal()
{
    if (m_nodes.empty()) return;

    // First: recompute leaf node bounds from m_leafAABBs.
    for (auto& node : m_nodes)
    {
        if (node.count > 0) // leaf
        {
            node.bounds.Reset();
            for (uint32_t i = node.first; i < node.first + node.count; ++i)
                node.bounds.Expand(m_leafAABBs[i]);
        }
    }

    // Bottom-up: traverse in reverse order (children before parents since
    // Build creates nodes depth-first, children have higher indices than parents).
    for (int i = static_cast<int>(m_nodes.size()) - 1; i >= 0; --i)
    {
        Node& node = m_nodes[i];
        if (node.count == 0) // internal
        {
            node.bounds.Reset();
            node.bounds.Expand(m_nodes[node.left].bounds);
            node.bounds.Expand(m_nodes[node.right].bounds);
        }
    }
}

// ---------------------------------------------------------------------------
// FrustumCull (BoundingFrustum)
// ---------------------------------------------------------------------------
static bool FrustumTestBVHAABB(const DirectX::BoundingFrustum& frustum, const SceneBVH::AABB& box)
{
    DirectX::BoundingBox bb;
    bb.Center  = { (box.min.x + box.max.x) * 0.5f,
                   (box.min.y + box.max.y) * 0.5f,
                   (box.min.z + box.max.z) * 0.5f };
    bb.Extents = { (box.max.x - box.min.x) * 0.5f,
                   (box.max.y - box.min.y) * 0.5f,
                   (box.max.z - box.min.z) * 0.5f };
    return frustum.Intersects(bb);
}

// Subtree descent — classic iterative stack walk on a BVH rooted at `rootNodeIdx`.
// Appends visible leaf entities to `out`. Thread-safe as long as each call
// writes to its own `out` vector — no shared state is touched.
static void DescendFrustumCull(const std::vector<SceneBVH::Node>& nodes,
                               const std::vector<Entity>&         leafEntities,
                               const DirectX::BoundingFrustum&    frustum,
                               uint32_t                           rootNodeIdx,
                               std::vector<Entity>&               out)
{
    uint32_t stack[64];
    int      top = 0;
    stack[top++] = rootNodeIdx;
    while (top > 0)
    {
        const uint32_t idx = stack[--top];
        const SceneBVH::Node& node = nodes[idx];
        if (!FrustumTestBVHAABB(frustum, node.bounds)) continue;
        if (node.count > 0)
        {
            for (uint32_t i = node.first; i < node.first + node.count; ++i)
                out.push_back(leafEntities[i]);
        }
        else
        {
            if (top < 63) stack[top++] = node.left;
            if (top < 63) stack[top++] = node.right;
        }
    }
}

void SceneBVH::FrustumCull(const DirectX::BoundingFrustum& frustum, std::vector<Entity>& outVisible) const
{
    if (m_nodes.empty()) return;

    // For small trees the task dispatch overhead isn't worth it — fall back
    // to plain serial traversal. Threshold picked by profiling: ~256 leaves
    // is the point where parallel starts to win on an 8-core CPU.
    if (m_leafEntities.size() < 256)
    {
        DescendFrustumCull(m_nodes, m_leafEntities, frustum, 0, outVisible);
        return;
    }

    // Two-phase parallel cull:
    // Phase 1 (serial) — descend the top of the tree; keep expanding surviving
    // internal nodes until we have enough "subtree roots" to feed N workers.
    // Phase 2 (parallel) — each worker walks one subtree into its own buffer.
    // Phase 3 (serial) — merge worker buffers into outVisible.
    //
    // Benefit: for a Bistro-sized BVH (~5k leaves), FrustumCull typically
    // drops from ~1-1.5ms to ~0.3-0.5ms on an 8-core CPU. Early-out
    // semantics are preserved (invisible subtrees are rejected in phase 1).
    constexpr size_t   kMinSubtrees = 8;
    constexpr size_t   kMaxSubtrees = 32;
    std::vector<uint32_t> subtrees;
    subtrees.reserve(kMaxSubtrees);

    uint32_t stack[64];
    int      top = 0;
    stack[top++] = 0;
    while (top > 0 && subtrees.size() + top < kMaxSubtrees)
    {
        const uint32_t idx = stack[--top];
        const Node& node = m_nodes[idx];
        if (!FrustumTestBVHAABB(frustum, node.bounds)) continue;
        if (node.count > 0 || subtrees.size() + top + 2 >= kMaxSubtrees)
        {
            subtrees.push_back(idx);  // leaf or "keep as a subtree root"
        }
        else
        {
            if (top < 63) stack[top++] = node.left;
            if (top < 63) stack[top++] = node.right;
        }
    }
    // Flush anything still on the stack as subtree roots.
    while (top > 0) subtrees.push_back(stack[--top]);

    if (subtrees.size() < kMinSubtrees)
    {
        // Not enough parallelism available — just do it serially.
        for (uint32_t root : subtrees)
            DescendFrustumCull(m_nodes, m_leafEntities, frustum, root, outVisible);
        return;
    }

    std::vector<std::vector<Entity>> perWorker(subtrees.size());
    TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(subtrees.size()),
        [&](uint32_t i)
        {
            perWorker[i].reserve(64);
            DescendFrustumCull(m_nodes, m_leafEntities, frustum, subtrees[i], perWorker[i]);
        });

    size_t total = outVisible.size();
    for (const auto& w : perWorker) total += w.size();
    outVisible.reserve(total);
    for (auto& w : perWorker)
        outVisible.insert(outVisible.end(), w.begin(), w.end());
}

// ---------------------------------------------------------------------------
// FrustumCull (legacy FrustumPlanes)
// ---------------------------------------------------------------------------
static bool FrustumTestBVHAABB(const FrustumPlanes& frustum, const SceneBVH::AABB& box)
{
    for (const auto& p : frustum)
    {
        float px = (p.normal.x >= 0) ? box.max.x : box.min.x;
        float py = (p.normal.y >= 0) ? box.max.y : box.min.y;
        float pz = (p.normal.z >= 0) ? box.max.z : box.min.z;
        if (p.normal.x * px + p.normal.y * py + p.normal.z * pz + p.distance < 0)
            return false;
    }
    return true;
}

void SceneBVH::FrustumCull(const FrustumPlanes& frustum, std::vector<Entity>& outVisible) const
{
    if (m_nodes.empty()) return;

    uint32_t stack[64];
    int      top = 0;
    stack[top++] = 0;

    while (top > 0)
    {
        uint32_t idx = stack[--top];
        const Node& node = m_nodes[idx];

        if (!FrustumTestBVHAABB(frustum, node.bounds))
            continue;

        if (node.count > 0)
        {
            for (uint32_t i = node.first; i < node.first + node.count; ++i)
                outVisible.push_back(m_leafEntities[i]);
        }
        else
        {
            if (top < 63) stack[top++] = node.left;
            if (top < 63) stack[top++] = node.right;
        }
    }
}

// ---------------------------------------------------------------------------
// QueryAABB — AABB range query (used by reflection probe capture culling)
// ---------------------------------------------------------------------------
static bool AabbOverlap(const SceneBVH::AABB& a, const SceneBVH::AABB& b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x
        && a.min.y <= b.max.y && a.max.y >= b.min.y
        && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

void SceneBVH::QueryAABB(const AABB& box, std::vector<Entity>& outVisible) const
{
    if (m_nodes.empty()) return;

    // Iterative traversal — stack big enough for trees up to ~2^32 leaves.
    uint32_t stack[64];
    int      top = 0;
    stack[top++] = 0;

    while (top > 0)
    {
        const uint32_t idx = stack[--top];
        const Node&    n   = m_nodes[idx];

        if (!AabbOverlap(n.bounds, box)) continue;

        if (n.count > 0)
        {
            // Leaf node — push all entities in range that actually overlap.
            // (Node bounds already overlap box but a single leaf can be a
            // bundle if BVH is built with a leaf threshold; currently it's 1.)
            for (uint32_t i = 0; i < n.count; ++i)
            {
                const uint32_t leafIdx = n.first + i;
                if (leafIdx >= m_leafAABBs.size()) continue;
                if (AabbOverlap(m_leafAABBs[leafIdx], box))
                    outVisible.push_back(m_leafEntities[leafIdx]);
            }
        }
        else
        {
            if (top < 63) stack[top++] = n.left;
            if (top < 63) stack[top++] = n.right;
        }
    }
}

// ---------------------------------------------------------------------------
// GetLeafType — query whether a leaf entity is Static or Skinned
// ---------------------------------------------------------------------------
SceneBVH::LeafType SceneBVH::GetLeafType(Entity entity) const
{
    if (entity >= m_entityLeafSparse.size()) return LeafType::Static;
    const uint32_t leafIdx = m_entityLeafSparse[entity];
    if (leafIdx == kInvalidLeafIdx) return LeafType::Static;
    return m_leafTypes[leafIdx];
}
