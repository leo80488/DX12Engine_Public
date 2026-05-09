#pragma once

// BTAsset — shared, parsed Behavior Tree resource.
//
// Same role as SkeletonAsset / ClipAsset / MeshLibRef in this engine:
// one parsed tree, many entities reference it via shared_ptr<BTAsset>.
// Per-entity execution state lives in BTInstance — see BTNode.h.

#include "AI/BTNode.h"
#include <memory>
#include <string>

namespace AI
{
    class BTAsset
    {
    public:
        std::unique_ptr<BTNode> root;

        // Source path the asset was parsed from. Empty for trees built
        // programmatically via BuildTestTree(). Used for hot-reload key
        // matching and editor display.
        std::string sourcePath;

        // Total node count (root + descendants). Populated by AssignIds.
        // Lets BTInstance reserve hashmap capacity ahead of first tick.
        uint32_t nodeCount = 0;

        // Walk the tree in DFS order and assign every node a unique
        // 1-based NodeId. Call once after parse / construction. Must be
        // re-invoked if the tree is mutated (only the loader does that).
        void AssignIds();

        // DFS lookup by id — used by the editor inspector to render trace
        // entries with their owning node label. O(N) walk; fine for a
        // debug panel that runs at ImGui rate.
        const BTNode* FindNode(NodeId id) const;
    };
}
