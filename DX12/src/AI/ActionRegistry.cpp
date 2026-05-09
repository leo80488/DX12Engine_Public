#include "AI/ActionRegistry.h"
#include "System/Log.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace AI
{
    void ActionRegistry::RegisterAction(std::string name, NativeHandler fn)
    {
        m_actions[std::move(name)] = std::move(fn);
    }

    void ActionRegistry::RegisterCondition(std::string name, NativeHandler fn)
    {
        m_conditions[std::move(name)] = std::move(fn);
    }

    bool ActionRegistry::HasNativeAction(const std::string& name) const
    {
        return m_actions.find(name) != m_actions.end();
    }

    bool ActionRegistry::HasNativeCondition(const std::string& name) const
    {
        return m_conditions.find(name) != m_conditions.end();
    }

    NodeStatus ActionRegistry::DispatchAction(const std::string& name,
                                              BTContext& ctx,
                                              const ActionParams& params)
    {
        auto it = m_actions.find(name);
        if (it != m_actions.end()) return it->second(ctx, params);
        return CallLua("Actions", name, ctx, params, m_warnedActions);
    }

    NodeStatus ActionRegistry::DispatchCondition(const std::string& name,
                                                 BTContext& ctx,
                                                 const ActionParams& params)
    {
        auto it = m_conditions.find(name);
        if (it != m_conditions.end()) return it->second(ctx, params);
        return CallLua("Conditions", name, ctx, params, m_warnedConditions);
    }

    // ---- Lua fallback ------------------------------------------------------
    // Reads the named global table (Actions / Conditions), looks up the
    // function by `name`, and invokes it with (ctx_userdata, params_table).
    // Result mapping:
    //   "Success" / true  → Success
    //   "Failure" / false → Failure
    //   "Running"         → Running
    //   anything else     → Failure (with one-shot warn)
    //
    // Forward-declared helper from LuaBTBindings.cpp (P4) — that
    // translation unit defines BTContext as a sol::usertype and exposes
    // params conversion. Implemented as a free function so this file
    // doesn't need to know the userdata layout.
    extern NodeStatus LuaInvokeBT(sol::state& lua,
                                  const char* tableName,
                                  const std::string& name,
                                  BTContext& ctx,
                                  const ActionParams& params,
                                  bool& outFoundCallable);

    NodeStatus ActionRegistry::CallLua(const char* tableName,
                                       const std::string& name,
                                       BTContext& ctx,
                                       const ActionParams& params,
                                       std::unordered_map<std::string, bool>& warnSet)
    {
        if (!m_lua)
        {
            if (!warnSet[name])
            {
                LOG_WARNING("BT %s '%s' not found (Lua disabled)",
                            tableName, name.c_str());
                warnSet[name] = true;
            }
            return NodeStatus::Failure;
        }

        bool found = false;
        const NodeStatus s = LuaInvokeBT(*m_lua, tableName, name, ctx, params, found);
        if (!found && !warnSet[name])
        {
            LOG_WARNING("BT %s '%s' not found in C++ or Lua",
                        tableName, name.c_str());
            warnSet[name] = true;
        }
        return s;
    }
}
