#include "AI/BTAsset.h"

#include <vector>

namespace AI
{
    void BTAsset::AssignIds()
    {
        nodeCount = 0;
        if (!root) return;

        std::vector<BTNode*> stack;
        stack.push_back(root.get());

        while (!stack.empty())
        {
            BTNode* n = stack.back();
            stack.pop_back();
            n->id = ++nodeCount;
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back(it->get());
        }
    }

    const BTNode* BTAsset::FindNode(NodeId id) const
    {
        if (!root || id == kInvalidNodeId) return nullptr;

        std::vector<const BTNode*> stack;
        stack.push_back(root.get());
        while (!stack.empty())
        {
            const BTNode* n = stack.back();
            stack.pop_back();
            if (n->id == id) return n;
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back(it->get());
        }
        return nullptr;
    }
}
