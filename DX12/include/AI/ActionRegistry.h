#pragma once

// ActionRegistry — name → handler dispatch for ActionLeaf / ConditionLeaf.
//
// Resolution order (matches the architecture doc §4.2):
//   1. Native C++ handler registered via Register{Action,Condition}.
//   2. Lua fallback — looks up Actions[name] / Conditions[name] in the
//      shared sol::state owned by ScriptSystem.
//
// Native handlers should be used for high-frequency / perf-critical work
// (movement, distance checks, perception queries). Lua is the right home
// for designer-iterated logic — boss patterns, encounter scripting.
//
// The registry does not own the Lua state — it borrows ScriptSystem's,
// so hot-reload, AfterDelay, EventBus bridging, TimeScale all work
// uniformly inside Lua actions.

#include "AI/BTNode.h"      // NodeStatus, BTContext
#include "AI/AIComponents.h" // BBValue

#include <functional>
#include <string>
#include <unordered_map>

namespace sol { class state; }

namespace AI
{
    using ActionParams  = std::unordered_map<std::string, BBValue>;
    using NativeHandler = std::function<NodeStatus(BTContext&, const ActionParams&)>;

    class ActionRegistry
    {
    public:
        // Optional Lua state for fallback dispatch. Pass the same sol::state
        // ScriptSystem owns; passing nullptr disables Lua dispatch entirely.
        // Borrowed — the registry never deletes it.
        void BindLua(sol::state* lua) { m_lua = lua; }
        sol::state* GetLua() const    { return m_lua; }

        void RegisterAction   (std::string name, NativeHandler fn);
        void RegisterCondition(std::string name, NativeHandler fn);

        // Dispatchers — invoked by ActionLeaf / ConditionLeaf. Returns
        // NodeStatus::Failure with a one-shot log if the name is unknown.
        NodeStatus DispatchAction   (const std::string& name,
                                     BTContext&         ctx,
                                     const ActionParams& params);
        NodeStatus DispatchCondition(const std::string& name,
                                     BTContext&         ctx,
                                     const ActionParams& params);

        // Inspector helpers — surfaced for the editor's "available actions"
        // listbox and unit-test ergonomics.
        bool HasNativeAction   (const std::string& name) const;
        bool HasNativeCondition(const std::string& name) const;

    private:
        std::unordered_map<std::string, NativeHandler> m_actions;
        std::unordered_map<std::string, NativeHandler> m_conditions;
        sol::state*                                    m_lua = nullptr;

        // One-shot log dedup: BT loops at 10 Hz, so a misspelled action
        // would otherwise spam the log thousands of times per second.
        std::unordered_map<std::string, bool> m_warnedActions;
        std::unordered_map<std::string, bool> m_warnedConditions;

        NodeStatus CallLua(const char*         tableName,
                           const std::string&  name,
                           BTContext&          ctx,
                           const ActionParams& params,
                           std::unordered_map<std::string, bool>& warnSet);
    };
}
