#pragma once

// SceneBVH — binary bounding volume hierarchy for CPU frustum culling.
//
// Supports two entity types:
//   - Static mesh: AABB from LocalAabb + GlobalTransform (stable, good for SAH)
//   - Skinned mesh: AABB from bone positions (changes every frame, leaf-refit only)
//
// Usage:
//   Frame 0:     Build(entities, aabbs, count)     — full SAH rebuild
//   Frame 1..N:  RefitLeaf(index, newAabb)          — update individual leaf AABBs
//                RefitInternal()                     — bottom-up refit node bounds
//   Periodic:    Build(...)                          — full rebuild when topology changes
//
// FrustumCull() returns visible entity IDs via BoundingFrustum intersection.

#include "ECS/ECS.h"
#include "Graphics/RenderTypes.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <vector>
#include <unordered_map>

class SceneBVH
{
public:
    // Leaf type: determines per-frame update strategy.
    //   Static  — AABB only changes when the entity moves (transform dirty).
    //   Skinned — AABB changes every frame (bone animation).
    enum class LeafType : uint8_t { Static = 0, Skinned = 1 };

    struct AABB
    {
        DirectX::XMFLOAT3 min = {  1e30f,  1e30f,  1e30f };
        DirectX::XMFLOAT3 max = { -1e30f, -1e30f, -1e30f };

        void Expand(const AABB& other)
        {
            min.x = (std::min)(min.x, other.min.x);
            min.y = (std::min)(min.y, other.min.y);
            min.z = (std::min)(min.z, other.min.z);
            max.x = (std::max)(max.x, other.max.x);
            max.y = (std::max)(max.y, other.max.y);
            max.z = (std::max)(max.z, other.max.z);
        }

        void Reset()
        {
            min = {  1e30f,  1e30f,  1e30f };
            max = { -1e30f, -1e30f, -1e30f };
        }

        float SurfaceArea() const
        {
            float dx = max.x - min.x;
            float dy = max.y - min.y;
            float dz = max.z - min.z;
            return 2.0f * (dx*dy + dy*dz + dz*dx);
        }

        DirectX::XMFLOAT3 Center() const
        {
            return { (min.x+max.x)*0.5f, (min.y+max.y)*0.5f, (min.z+max.z)*0.5f };
        }
    };

    // Full SAH rebuild from scratch. Call when entities are added/removed.
    // types[] is optional — if null, all leaves default to Static.
    void Build(const Entity* entities, const AABB* aabbs, const LeafType* types, uint32_t count);

    // Update a single leaf's AABB by entity ID (for skinned mesh refit).
    void RefitLeaf(Entity entity, const AABB& newAabb);

    // Bottom-up refit all internal node bounds after leaf changes.
    void RefitInternal();

    // Query leaf type by entity ID.
    LeafType GetLeafType(Entity entity) const;

    // Traverse and return visible entity IDs.
    void FrustumCull(const DirectX::BoundingFrustum& frustum, std::vector<Entity>& outVisible) const;
    void FrustumCull(const FrustumPlanes& frustum, std::vector<Entity>& outVisible) const;

    // AABB range query — returns every leaf whose bounds intersect `box`.
    // Used by the reflection probe capture pipeline to limit per-probe draws
    // to geometry inside the probe's influence bounds (outer falloff AABB).
    // Serial traversal only — probe bake is already amortized to 1 per frame
    // so the cost of a parallel dispatch wouldn't pay back.
    void QueryAABB(const AABB& box, std::vector<Entity>& outVisible) const;

    uint32_t GetNodeCount()  const { return static_cast<uint32_t>(m_nodes.size()); }
    uint32_t GetLeafCount()  const { return static_cast<uint32_t>(m_leafEntities.size()); }
    bool     NeedsRebuild()  const { return m_needsRebuild; }
    void     MarkRebuild()         { m_needsRebuild = true; }

    // Node is exposed publicly so the parallel cull helper in SceneBVH.cpp
    // can reference it — keeping the type private would force that helper
    // into the class or into a friend declaration.
    struct Node
    {
        AABB     bounds;
        uint32_t left    = 0;
        uint32_t right   = 0;
        uint32_t first   = 0;     // first leaf index (leaf only)
        uint32_t count   = 0;     // leaf count (0 = internal)
        uint32_t parent  = ~0u;   // parent node index (for bottom-up refit)
    };

private:
    std::vector<Node>     m_nodes;
    std::vector<Entity>   m_leafEntities;
    std::vector<AABB>     m_leafAABBs;
    std::vector<LeafType> m_leafTypes;
    bool                  m_needsRebuild = true;

    // Entity → leaf index map for fast RefitLeaf lookup.
    // Sparse array indexed directly by Entity ID — two array loads per
    // lookup instead of unordered_map's hash + bucket walk. Entry value
    // `kInvalidLeafIdx` means "entity not in BVH". ~N uint32 = 4N bytes
    // of waste for dense Entity IDs; trivial on Bistro-scale scenes.
    static constexpr uint32_t           kInvalidLeafIdx = ~0u;
    std::vector<uint32_t>               m_entityLeafSparse;

    uint32_t BuildRecursive(uint32_t begin, uint32_t end, uint32_t parentIdx);
};
