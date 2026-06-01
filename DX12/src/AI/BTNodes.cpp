#include "AI/BTNodes.h"
#include "AI/BTAsset.h"
#include "AI/ActionRegistry.h"

#include "System/Log.h"

#include <cmath>

namespace AI
{
    // Tracing helper — logs a node's status into the per-instance trace
    // buffer so the editor visualiser can highlight the path. Skips when
    // there is no trace buffer (e.g. unit tests build a context without
    // an instance).
    static void Record(const BTContext& ctx, NodeId id, NodeStatus s)
    {
        if (ctx.instance) ctx.instance->trace.push_back({ id, s });
    }

    // ===================================================================
    // Sequence
    // ===================================================================
    NodeStatus Sequence::Tick(BTContext& ctx)
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            const NodeStatus s = children[i]->Tick(ctx);
            if (s == NodeStatus::Failure || s == NodeStatus::Running)
            {
                Record(ctx, id, s);
                return s;
            }
        }
        Record(ctx, id, NodeStatus::Success);
        return NodeStatus::Success;
    }

    // ===================================================================
    // Selector
    // ===================================================================
    NodeStatus Selector::Tick(BTContext& ctx)
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            const NodeStatus s = children[i]->Tick(ctx);
            if (s == NodeStatus::Success || s == NodeStatus::Running)
            {
                Record(ctx, id, s);
                return s;
            }
        }
        Record(ctx, id, NodeStatus::Failure);
        return NodeStatus::Failure;
    }

    // ===================================================================
    // Parallel
    // ===================================================================
    NodeStatus Parallel::Tick(BTContext& ctx)
    {
        const uint32_t total      = static_cast<uint32_t>(children.size());
        const uint32_t needSucc   = succeedOn == 0 ? total : succeedOn;
        const uint32_t needFail   = failOn    == 0 ? 1u    : failOn;

        uint32_t succ = 0, fail = 0;
        for (size_t i = 0; i < children.size(); ++i)
        {
            const NodeStatus s = children[i]->Tick(ctx);
            if      (s == NodeStatus::Success) ++succ;
            else if (s == NodeStatus::Failure) ++fail;
        }

        NodeStatus out = NodeStatus::Running;
        if      (fail >= needFail) out = NodeStatus::Failure;
        else if (succ >= needSucc) out = NodeStatus::Success;
        Record(ctx, id, out);
        return out;
    }

    // ===================================================================
    // Inverter
    // ===================================================================
    NodeStatus Inverter::Tick(BTContext& ctx)
    {
        if (children.empty()) { Record(ctx, id, NodeStatus::Failure); return NodeStatus::Failure; }
        const NodeStatus s = children[0]->Tick(ctx);
        NodeStatus out = s;
        if      (s == NodeStatus::Success) out = NodeStatus::Failure;
        else if (s == NodeStatus::Failure) out = NodeStatus::Success;
        Record(ctx, id, out);
        return out;
    }

    // ===================================================================
    // Repeater
    // ===================================================================
    NodeStatus Repeater::Tick(BTContext& ctx)
    {
        if (children.empty()) { Record(ctx, id, NodeStatus::Success); return NodeStatus::Success; }

        // Re-tick the child every frame; on Success, bump per-instance
        // count and check whether we hit the target.
        const NodeStatus s = children[0]->Tick(ctx);

        if (s == NodeStatus::Running)
        {
            Record(ctx, id, NodeStatus::Running);
            return NodeStatus::Running;
        }

        // count == 0  => infinite loop, never report success
        if (count == 0)
        {
            Record(ctx, id, NodeStatus::Running);
            return NodeStatus::Running;
        }

        if (!ctx.instance)
        {
            // No instance to track count — degenerate to single-shot.
            Record(ctx, id, s);
            return s;
        }

        uint32_t& visited = ctx.instance->repeats[id];
        ++visited;
        if (visited >= count)
        {
            visited = 0;
            Record(ctx, id, NodeStatus::Success);
            return NodeStatus::Success;
        }
        Record(ctx, id, NodeStatus::Running);
        return NodeStatus::Running;
    }

    // ===================================================================
    // Cooldown
    // ===================================================================
    NodeStatus Cooldown::Tick(BTContext& ctx)
    {
        if (children.empty()) { Record(ctx, id, NodeStatus::Failure); return NodeStatus::Failure; }

        if (ctx.instance)
        {
            float& timer = ctx.instance->cooldowns[id];
            if (timer > 0.f)
            {
                timer -= ctx.deltaTime;
                Record(ctx, id, NodeStatus::Failure);
                return NodeStatus::Failure;
            }

            const NodeStatus s = children[0]->Tick(ctx);
            if (s == NodeStatus::Success)
                timer = seconds;
            Record(ctx, id, s);
            return s;
        }

        // No instance → behave as a passthrough.
        const NodeStatus s = children[0]->Tick(ctx);
        Record(ctx, id, s);
        return s;
    }

    // ===================================================================
    // BlackboardCondition
    // ===================================================================
    bool BlackboardCondition::Evaluate(const BlackboardComponent& bb) const
    {
        const auto it = bb.values.find(key);
        if (it == bb.values.end()) return op == BBCmpOp::NotEquals; // missing != anything

        if (op == BBCmpOp::IsSet) return true;

        if (!expected.has_value()) return false;

        // Custom variant equality: XMFLOAT3 has no operator==, so the
        // default std::variant operator== does not compile. Visit both
        // values; same-type pairs compare component-wise (vec3) or via
        // == (everything else); type mismatch is never equal.
        const bool eq = std::visit([](const auto& a, const auto& b) -> bool {
            using A = std::decay_t<decltype(a)>;
            using B = std::decay_t<decltype(b)>;
            if constexpr (!std::is_same_v<A, B>) return false;
            else if constexpr (std::is_same_v<A, DirectX::XMFLOAT3>)
                return a.x == b.x && a.y == b.y && a.z == b.z;
            else if constexpr (std::is_same_v<A, std::vector<DirectX::XMFLOAT3>>)
            {
                // std::vector<XMFLOAT3> has no operator== (XMFLOAT3 lacks one).
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z)
                        return false;
                return true;
            }
            else
                return a == b;
        }, it->second, *expected);
        return op == BBCmpOp::Equals ? eq : !eq;
    }

    NodeStatus BlackboardCondition::Tick(BTContext& ctx)
    {
        if (!ctx.blackboard)
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        if (!Evaluate(*ctx.blackboard))
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        if (children.empty())
        {
            Record(ctx, id, NodeStatus::Success);
            return NodeStatus::Success;
        }

        const NodeStatus s = children[0]->Tick(ctx);
        Record(ctx, id, s);
        return s;
    }

    // ===================================================================
    // ActionLeaf
    // ===================================================================
    NodeStatus ActionLeaf::Tick(BTContext& ctx)
    {
        NodeStatus s = NodeStatus::Failure;
        if (ctx.actions)
            s = ctx.actions->DispatchAction(actionName, ctx, params);
        Record(ctx, id, s);
        if (s == NodeStatus::Running && ctx.instance)
            ctx.instance->runningNode = id;
        return s;
    }

    // ===================================================================
    // ConditionLeaf
    // ===================================================================
    NodeStatus ConditionLeaf::Tick(BTContext& ctx)
    {
        NodeStatus s = NodeStatus::Failure;
        if (ctx.actions)
            s = ctx.actions->DispatchCondition(conditionName, ctx, params);

        if (s == NodeStatus::Running)
        {
            // Conditions must be pure. Coerce to Failure with a one-shot
            // log; ActionRegistry already deduplicates per-name warnings
            // so we can rely on it here.
            LOG_WARNING("BT condition '%s' returned Running — coerced to Failure",
                        conditionName.c_str());
            s = NodeStatus::Failure;
        }
        Record(ctx, id, s);
        return s;
    }

    // ===================================================================
    // UtilitySelector
    // ===================================================================
    NodeStatus UtilitySelector::Tick(BTContext& ctx)
    {
        if (children.empty())
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        int   bestIdx   = -1;
        float bestScore = -1.f;
        for (size_t i = 0; i < children.size(); ++i)
        {
            float score = 0.f;
            if (i < childScores.size() && childScores[i])
                score = childScores[i](ctx);
            if (std::isnan(score) || score < 0.f) score = 0.f;
            if (score > bestScore)
            {
                bestScore = score;
                bestIdx   = static_cast<int>(i);
            }
        }
        if (bestIdx < 0 || bestScore <= 0.f)
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        const NodeStatus s = children[bestIdx]->Tick(ctx);
        Record(ctx, id, s);
        return s;
    }

    // ===================================================================
    // SubTree
    // ===================================================================
    NodeStatus SubTree::Tick(BTContext& ctx)
    {
        if (!tree || !tree->root)
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }
        const NodeStatus s = tree->root->Tick(ctx);
        Record(ctx, id, s);
        return s;
    }
}
