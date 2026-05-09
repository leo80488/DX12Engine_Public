#pragma once

// Concrete BT node types — composites, decorators, leaves.
//
// All per-entity state (running-child index, cooldown timers, repeat
// counters, current utility scores) lives in BTInstance, keyed by NodeId.
// Nothing here can be made non-const-tickable without breaking the shared
// tree contract.

#include "AI/BTNode.h"
#include "AI/AIComponents.h"     // BBValue

#include <functional>
#include <optional>
#include <string>

namespace AI
{
    // ===================================================================
    // Composites
    // ===================================================================

    // Sequence — run children left-to-right. Stops on the first Failure or
    // Running. Returns Success only if every child succeeds.
    //
    // No per-instance "current child" index: when a child returns Running
    // we re-enter from child 0 next tick. Children that need to remember
    // their progress must do it themselves (typically via blackboard).
    // This is the pragmatic choice for tick-based BTs and matches the
    // convention used by most middleware out of the box; if a long
    // sequence becomes a problem in practice, factor it into a sub-tree
    // that remembers its phase via a blackboard key.
    class Sequence final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Sequence"; }
    };

    // Selector — run children left-to-right. Stops on the first Success
    // or Running. Returns Failure only if every child fails.
    class Selector final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Selector"; }
    };

    // Parallel — tick every child every tick.
    //
    // Termination policy:
    //   succeedOn — minimum number of Success returns to declare overall
    //               Success (default = all children)
    //   failOn    — minimum number of Failure returns to declare overall
    //               Failure (default = 1, the conventional "any fail
    //               kills the parallel" policy)
    //
    // A Running result that doesn't yet trip either threshold yields
    // Running. Mirroring most middleware behaviour, Parallel does not
    // remember which children "already succeeded" — it ticks every child
    // every frame. Children that need one-shot semantics should gate
    // themselves on a blackboard flag.
    class Parallel final : public BTNode
    {
    public:
        uint32_t succeedOn = 0;   // 0 ⇒ require ALL children to succeed
        uint32_t failOn    = 1;   // any single failure kills the parallel

        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Parallel"; }
    };

    // ===================================================================
    // Decorators (always have exactly one child)
    // ===================================================================

    class Inverter final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Inverter"; }
    };

    // Repeater — tick the child up to `count` times. count == 0 means
    // infinite (re-tick child every frame; the parent never sees Success
    // until something else aborts).
    class Repeater final : public BTNode
    {
    public:
        uint32_t count = 1;

        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Repeater"; }
    };

    // Cooldown — once the child returns Success, block further executions
    // for `seconds` of game time. While blocked, the decorator returns
    // Failure (never Running) so a parent Selector can fall through to a
    // sibling.
    class Cooldown final : public BTNode
    {
    public:
        float seconds = 1.f;

        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Cooldown"; }
    };

    // BlackboardCondition — gate the child on a blackboard predicate.
    //
    // Three operator modes:
    //   IsSet      — pass iff the key exists
    //   Equals     — pass iff the value equals `expected`
    //   NotEquals  — pass iff the value differs from `expected`
    enum class BBCmpOp : uint8_t { IsSet, Equals, NotEquals };

    class BlackboardCondition final : public BTNode
    {
    public:
        std::string             key;
        BBCmpOp                 op = BBCmpOp::IsSet;
        std::optional<BBValue>  expected;

        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "BlackboardCondition"; }

    private:
        bool Evaluate(const BlackboardComponent& bb) const;
    };

    // ===================================================================
    // Leaves
    // ===================================================================

    // ActionLeaf — looks up a name in ActionRegistry and dispatches.
    // Native handlers run in C++; missing names fall through to a Lua
    // table (Actions[name]). The optional `params` blob is a string-keyed
    // BBValue map that the registry forwards to the handler verbatim.
    class ActionLeaf final : public BTNode
    {
    public:
        std::string                                actionName;
        std::unordered_map<std::string, BBValue>   params;

        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Action"; }
    };

    // ConditionLeaf — same dispatch as ActionLeaf but pulls from the
    // condition table. Conditions MUST be pure: returning anything other
    // than Success / Failure is a parse-time error (Running collapses
    // to Failure with a one-shot warning).
    class ConditionLeaf final : public BTNode
    {
    public:
        std::string                                conditionName;
        std::unordered_map<std::string, BBValue>   params;

        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Condition"; }
    };

    // ===================================================================
    // Advanced (P8)
    // ===================================================================

    // UtilitySelector — like Selector, but each child reports a utility
    // score; the highest-scoring child is ticked. Scores are expected to
    // be in [0, +inf); negative or NaN scores are clamped to 0 and the
    // child is skipped. Score functions read from the blackboard (and
    // optionally world state) and must NOT mutate either.
    class UtilitySelector final : public BTNode
    {
    public:
        // childScores[i] returns the utility for children[i]. Use the
        // default (always-1) if you only want to plug in a single
        // dynamic-ranked child, e.g. for unit tests.
        std::vector<std::function<float(const BTContext&)>> childScores;

        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "UtilitySelector"; }
    };

    // SubTree — references another BTAsset. Owns no children of its own;
    // its Tick simply enters the subtree's root using the same context.
    // Per-instance state for the subtree is keyed by the SubTree's NodeId
    // plus the subtree node's own NodeIds — this is correct because BT
    // parses assign ids per asset and SubTree.id is unique within the
    // owning tree, so the (subtree_id, child_node_id) tuple keys are
    // namespaced via the BTInstance lookups already in use.
    //
    // The subtree's BTAsset is held as a shared_ptr so unloading the
    // outer tree does not invalidate it while another entity is still
    // mid-tick.
    class SubTree final : public BTNode
    {
    public:
        std::shared_ptr<class BTAsset> tree;

        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "SubTree"; }
    };
}
