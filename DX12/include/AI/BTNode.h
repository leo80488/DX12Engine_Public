#pragma once

// BTNode — base of every node type. Trees are pure data: shareable across
// entities, no per-entity state. All running-node tracking, cooldowns,
// repeat counts live in BTInstance, keyed by NodeId.
//
// Node lifetime: BTAsset owns the root unique_ptr; each composite/decorator
// owns its children via unique_ptr. Tick traversal uses raw pointers.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ECS/ECS.h"

class World;
struct BlackboardComponent;

namespace AI
{
    using NodeId = uint32_t;
    constexpr NodeId kInvalidNodeId = 0u;

    enum class NodeStatus : uint8_t
    {
        Success,
        Failure,
        Running,
        Invalid
    };

    struct BTInstance;
    class  ActionRegistry;

    // BTContext — passed by reference into every Tick. Holds the per-tick
    // ECS hooks plus the per-entity instance. Never store across frames.
    struct BTContext
    {
        Entity                entity     = NullEntity;
        World*                world      = nullptr;
        BlackboardComponent*  blackboard = nullptr;
        BTInstance*           instance   = nullptr;
        ActionRegistry*       actions    = nullptr;
        float                 deltaTime  = 0.f;
        float                 elapsed    = 0.f;
    };

    // BTInstance — per-entity runtime state. Lives on AIComponent. Shared
    // BTNode pointers are stable; this struct holds every value that would
    // otherwise have to live on the node.
    struct BTInstance
    {
        // Currently-running leaf — when a leaf returns Running, the next
        // tick should resume from this node rather than re-entering the
        // tree from the root. The composite walking code does NOT use this
        // for control flow; it is recorded for trace visualisation and as
        // a hook for future event-driven re-evaluation.
        NodeId runningNode = kInvalidNodeId;

        // Per-node cooldown timers (Cooldown decorator). Counts DOWN; the
        // decorator ticks the entry each frame and gates execution while
        // the value is positive.
        std::unordered_map<NodeId, float> cooldowns;

        // Per-node repeat counters (Repeater decorator). Counts UP toward
        // the per-node target.
        std::unordered_map<NodeId, uint32_t> repeats;

        // Trace for the editor BT visualiser — list of NodeIds visited on
        // the most recent tick, plus the status returned by each. Cleared
        // at the start of every Tick that runs (see AISystem::Update).
        struct TraceEntry { NodeId id; NodeStatus status; };
        std::vector<TraceEntry> trace;

        // When non-zero, the next tick begins a fresh traversal regardless
        // of what runningNode says. Set by hot-reload to flush any stale
        // pointers into the previous BTAsset.
        bool resetRequested = false;
    };

    // ---- BTNode -----------------------------------------------------------
    class BTNode
    {
    public:
        virtual ~BTNode() = default;

        virtual NodeStatus Tick(BTContext& ctx) = 0;

        // OnEnter / OnExit are advisory hooks — base node uses them only
        // for trace bookkeeping. Composites override OnExit when they need
        // to clear per-node state on abort (e.g. Sequence resets its
        // running-child index when a child fails).
        virtual void OnEnter(BTContext& /*ctx*/) {}
        virtual void OnExit (BTContext& /*ctx*/, NodeStatus /*status*/) {}

        // Display name — used by the editor trace panel and error logs.
        // Override in concrete leaves to surface action / condition names.
        virtual const char* TypeName() const = 0;

        // Children — composite/decorator subclasses populate this; leaves
        // leave it empty. Owned via unique_ptr; raw pointer access through
        // GetChild() during traversal.
        std::vector<std::unique_ptr<BTNode>> children;

        BTNode* GetChild(size_t i) const
        {
            return i < children.size() ? children[i].get() : nullptr;
        }

        size_t ChildCount() const { return children.size(); }

        // Assigned by BTAsset::AssignIds at parse time (depth-first order).
        // Stable for the lifetime of the BTAsset; NEVER reuse for cross-
        // tree state because the tree pointer is what disambiguates entries
        // in BTInstance maps.
        NodeId id = kInvalidNodeId;

        // Optional designer-facing name (e.g. "Attack if target visible").
        // Surfaced by the trace visualiser; not used for any logic.
        std::string name;
    };
}
