#include "AI/LuaBTBindings.h"
#include "AI/BTNode.h"
#include "AI/AIComponents.h"
#include "AI/ActionRegistry.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "System/Log.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <DirectXMath.h>
#include <cmath>
#include <string>

namespace AI
{
    // -----------------------------------------------------------------------
    // BTContext userdata — Lua sees this as `ctx`. Methods read/write the
    // blackboard, query world-space positions, and surface delta time.
    //
    // Lifetime: the userdata is a *pointer* to a stack BTContext owned by
    // the C++ dispatcher. It is only valid for the duration of the Lua
    // callback. We never let Lua hold it across frames — handlers are
    // called synchronously, return, and the userdata is released before
    // the C++ dispatch returns.
    // -----------------------------------------------------------------------
    struct LuaCtxRef
    {
        BTContext* ptr = nullptr;
    };

    // BBValue → Lua object. XMFLOAT3 becomes {x,y,z} table.
    static sol::object PushBB(sol::state_view lua, const BBValue& v)
    {
        return std::visit([&](auto&& x) -> sol::object {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, DirectX::XMFLOAT3>)
            {
                sol::table t = lua.create_table();
                t["x"] = x.x; t["y"] = x.y; t["z"] = x.z;
                t[1]   = x.x; t[2]   = x.y; t[3]   = x.z;
                return sol::make_object(lua, t);
            }
            else if constexpr (std::is_same_v<T, Entity>)
            {
                return sol::make_object(lua, static_cast<uint32_t>(x));
            }
            else
            {
                return sol::make_object(lua, x);
            }
        }, v);
    }

    static BBValue PullBB(const sol::object& v)
    {
        if (v.is<bool>())                return v.as<bool>();
        if (v.is<int>())                 return v.as<int>();
        if (v.is<float>())               return v.as<float>();
        if (v.is<std::string>())         return v.as<std::string>();
        if (v.is<sol::table>())
        {
            sol::table t = v.as<sol::table>();
            DirectX::XMFLOAT3 f3{};
            // Accept both {x,y,z} (named) and array {x,y,z} (positional).
            // get_or without an explicit template parameter — sol2 has two
            // overloads of get_or<T> that both match when `T == default
            // type`; deducing both arguments avoids the ambiguity.
            if (t["x"].valid()) f3.x = t.get_or("x", 0.0f);
            else                f3.x = t.get_or(1,   0.0f);
            if (t["y"].valid()) f3.y = t.get_or("y", 0.0f);
            else                f3.y = t.get_or(2,   0.0f);
            if (t["z"].valid()) f3.z = t.get_or("z", 0.0f);
            else                f3.z = t.get_or(3,   0.0f);
            return f3;
        }
        return std::string{};
    }

    // ---- Userdata methods --------------------------------------------------
    static sol::object Ctx_GetBB(LuaCtxRef& self, sol::this_state ts, const std::string& key)
    {
        sol::state_view lua(ts);
        if (!self.ptr || !self.ptr->blackboard) return sol::lua_nil_t{};
        auto it = self.ptr->blackboard->values.find(key);
        if (it == self.ptr->blackboard->values.end()) return sol::lua_nil_t{};
        return PushBB(lua, it->second);
    }

    static void Ctx_SetBB(LuaCtxRef& self, const std::string& key, sol::object value)
    {
        if (!self.ptr || !self.ptr->blackboard) return;
        self.ptr->blackboard->values[key] = PullBB(value);
    }

    static bool Ctx_HasBB(LuaCtxRef& self, const std::string& key)
    {
        if (!self.ptr || !self.ptr->blackboard) return false;
        return self.ptr->blackboard->values.count(key) > 0;
    }

    static uint32_t Ctx_Entity(LuaCtxRef& self)
    {
        return self.ptr ? static_cast<uint32_t>(self.ptr->entity) : 0u;
    }

    static float Ctx_DeltaTime(LuaCtxRef& self)
    {
        return self.ptr ? self.ptr->deltaTime : 0.f;
    }

    static float Ctx_Elapsed(LuaCtxRef& self)
    {
        return self.ptr ? self.ptr->elapsed : 0.f;
    }

    // Read GlobalTransform translation column. Returns {x=,y=,z=}.
    static sol::table Ctx_GetEntityPosition(LuaCtxRef& self, sol::this_state ts, uint32_t entityId)
    {
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f;
        if (!self.ptr || !self.ptr->world) return t;
        const Entity e = static_cast<Entity>(entityId);
        if (auto* gt = self.ptr->world->GetComponent<GlobalTransform>(e))
        {
            t["x"] = gt->matrix._41;
            t["y"] = gt->matrix._42;
            t["z"] = gt->matrix._43;
        }
        return t;
    }

    // Distance between this entity and another.
    static float Ctx_DistanceTo(LuaCtxRef& self, uint32_t targetId)
    {
        if (!self.ptr || !self.ptr->world) return 0.f;
        const auto* a = self.ptr->world->GetComponent<GlobalTransform>(self.ptr->entity);
        const auto* b = self.ptr->world->GetComponent<GlobalTransform>(static_cast<Entity>(targetId));
        if (!a || !b) return 0.f;
        const float dx = a->matrix._41 - b->matrix._41;
        const float dy = a->matrix._42 - b->matrix._42;
        const float dz = a->matrix._43 - b->matrix._43;
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    void RegisterLuaBTBindings(sol::state& lua)
    {
        lua.new_usertype<LuaCtxRef>("BTContext",
            sol::no_constructor,
            "GetBB",              &Ctx_GetBB,
            "SetBB",              &Ctx_SetBB,
            "HasBB",              &Ctx_HasBB,
            "Entity",             &Ctx_Entity,
            "DeltaTime",          &Ctx_DeltaTime,
            "Elapsed",            &Ctx_Elapsed,
            "GetEntityPosition",  &Ctx_GetEntityPosition,
            "DistanceTo",         &Ctx_DistanceTo);

        // Designer-facing tables. Scripts append entries:
        //   Actions.MoveToTarget    = function(ctx, params) ... end
        //   Conditions.IsLowHealth  = function(ctx, params) ... end
        if (!lua["Actions"].valid())    lua["Actions"]    = lua.create_table();
        if (!lua["Conditions"].valid()) lua["Conditions"] = lua.create_table();

        // String constants so handlers can write `return BT.Success`
        // instead of stringly-typed magic. Both forms are accepted by
        // LuaInvokeBT below; the table form just makes typos compile-error.
        sol::table bt = lua.create_table();
        bt["Success"] = "Success";
        bt["Failure"] = "Failure";
        bt["Running"] = "Running";
        lua["BT"] = bt;
    }

    // -----------------------------------------------------------------------
    // LuaInvokeBT — called by ActionRegistry when no native handler exists.
    // -----------------------------------------------------------------------
    NodeStatus LuaInvokeBT(sol::state& lua,
                           const char* tableName,
                           const std::string& name,
                           BTContext& ctx,
                           const std::unordered_map<std::string, BBValue>& params,
                           bool& outFoundCallable)
    {
        outFoundCallable = false;
        sol::object tableObj = lua[tableName];
        if (!tableObj.is<sol::table>()) return NodeStatus::Failure;

        sol::table tbl = tableObj.as<sol::table>();
        sol::object fnObj = tbl[name];
        if (!fnObj.is<sol::protected_function>()) return NodeStatus::Failure;
        outFoundCallable = true;

        // Build per-call userdata wrapper for ctx and a Lua table for params.
        LuaCtxRef wrapper{ &ctx };
        sol::table paramsTbl = lua.create_table();
        for (const auto& kv : params)
            paramsTbl[kv.first] = PushBB(lua, kv.second);

        sol::protected_function fn = fnObj;
        sol::protected_function_result r = fn(wrapper, paramsTbl);
        if (!r.valid())
        {
            sol::error err = r;
            LOG_ERROR("BT %s.%s lua error: %s", tableName, name.c_str(), err.what());
            return NodeStatus::Failure;
        }

        sol::object out = r;
        if (out.is<bool>()) return out.as<bool>() ? NodeStatus::Success : NodeStatus::Failure;
        if (out.is<std::string>())
        {
            const std::string s = out.as<std::string>();
            if (s == "Success") return NodeStatus::Success;
            if (s == "Failure") return NodeStatus::Failure;
            if (s == "Running") return NodeStatus::Running;
        }
        return NodeStatus::Failure;
    }
}
