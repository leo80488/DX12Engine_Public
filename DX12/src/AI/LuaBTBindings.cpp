#include "AI/LuaBTBindings.h"
#include "AI/BTNode.h"
#include "AI/AIComponents.h"
#include "AI/ActionRegistry.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"  // GlobalTransform — entity world pose
#include "ECS/AnimationComponents.h"  // AnimationComponent for FindAnimatedDescendant
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

    // BBValue → Lua object. XMFLOAT3 becomes {x,y,z} table; vector<XMFLOAT3>
    // becomes a 1-indexed array of those tables (the inverse of LuaToBBValue
    // in AISystem.cpp). Entity is the raw uint32 so blackboard equality
    // checks stay number-based.
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
            else if constexpr (std::is_same_v<T, std::vector<DirectX::XMFLOAT3>>)
            {
                sol::table arr = lua.create_table();
                for (size_t i = 0; i < x.size(); ++i)
                {
                    sol::table t = lua.create_table();
                    t["x"] = x[i].x; t["y"] = x[i].y; t["z"] = x[i].z;
                    t[1]   = x[i].x; t[2]   = x[i].y; t[3]   = x[i].z;
                    arr[i + 1] = t;
                }
                return sol::make_object(lua, arr);
            }
            else if constexpr (std::is_same_v<T, std::vector<std::string>>)
            {
                sol::table arr = lua.create_table();
                for (size_t i = 0; i < x.size(); ++i)
                    arr[i + 1] = x[i];
                return sol::make_object(lua, arr);
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
            // Same disambiguation as AISystem::LuaToBBValue:
            //   first is table  → vector<XMFLOAT3>
            //   first is string → vector<string>
            //   else            → scalar XMFLOAT3
            sol::object first = t[1];
            if (first.is<sol::table>())
            {
                std::vector<DirectX::XMFLOAT3> arr;
                for (size_t i = 1; ; ++i)
                {
                    sol::object slot = t[i];
                    if (!slot.is<sol::table>()) break;
                    sol::table st = slot.as<sol::table>();
                    DirectX::XMFLOAT3 f{};
                    if (st["x"].valid()) f.x = st.get_or("x", 0.0f);
                    else                 f.x = st.get_or(1,   0.0f);
                    if (st["y"].valid()) f.y = st.get_or("y", 0.0f);
                    else                 f.y = st.get_or(2,   0.0f);
                    if (st["z"].valid()) f.z = st.get_or("z", 0.0f);
                    else                 f.z = st.get_or(3,   0.0f);
                    arr.push_back(f);
                }
                return arr;
            }
            if (first.is<std::string>())
            {
                std::vector<std::string> arr;
                for (size_t i = 1; ; ++i)
                {
                    sol::object slot = t[i];
                    if (!slot.is<std::string>()) break;
                    arr.push_back(slot.as<std::string>());
                }
                return arr;
            }
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

    // Delete a blackboard key. Lua `ctx:SetBB(key, nil)` does NOT remove the
    // entry — PullBB falls through nil to an empty std::string, so the key
    // hangs around with value "". Use RemoveBB when you want "make this key
    // not exist" semantics (consume-once flags like ActiveAttack /
    // HitReactStart). Returns true if a key was actually erased.
    static bool Ctx_RemoveBB(LuaCtxRef& self, const std::string& key)
    {
        if (!self.ptr || !self.ptr->blackboard) return false;
        return self.ptr->blackboard->values.erase(key) > 0;
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

    // Resolve world-space position for any entity from its GlobalTransform
    // (translation column, written by TransformSystem). The camera is an
    // ordinary ECS entity — its pose lives on GlobalTransform like everything
    // else — so ChaseCamera resolves through this same path.
    static DirectX::XMFLOAT3 ResolveEntityPos(const World& world, Entity e)
    {
        if (const auto* gt = world.GetComponent<GlobalTransform>(e))
            return { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
        return { 0.f, 0.f, 0.f };
    }

    static sol::table Ctx_GetEntityPosition(LuaCtxRef& self, sol::this_state ts, uint32_t entityId)
    {
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f;
        if (!self.ptr || !self.ptr->world) return t;
        const auto p = ResolveEntityPos(*self.ptr->world, static_cast<Entity>(entityId));
        t["x"] = p.x; t["y"] = p.y; t["z"] = p.z;
        return t;
    }

    // Distance between this entity and another.
    static float Ctx_DistanceTo(LuaCtxRef& self, uint32_t targetId)
    {
        if (!self.ptr || !self.ptr->world) return 0.f;
        const auto a = ResolveEntityPos(*self.ptr->world, self.ptr->entity);
        const auto b = ResolveEntityPos(*self.ptr->world, static_cast<Entity>(targetId));
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    // BFS the hierarchy under `id` and return the first entity (incl. `id`
    // itself) that owns an AnimationComponent. Returns 0 if none found.
    //
    // This mirrors Engine.FindAnimatedDescendant in ScriptSystem, but uses
    // BTContext's `world` pointer directly so it works during AISystem ticks
    // (Engine.* helpers depend on ScriptSystem::m_world which is only set
    // during ScriptSystem::Update — calling them from a BT action returns
    // garbage / 0).
    static uint32_t Ctx_FindAnimatedDescendant(LuaCtxRef& self, uint32_t id)
    {
        if (!self.ptr || !self.ptr->world) return 0u;
        const World& w = *self.ptr->world;
        const Entity start = static_cast<Entity>(id);
        if (!w.IsAlive(start)) return 0u;

        std::vector<Entity> stack;
        stack.push_back(start);
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (w.GetComponent<AnimationComponent>(e))
                return static_cast<uint32_t>(e);
            if (const auto* kids = w.GetComponent<Children>(e))
            {
                for (auto it = kids->entities.rbegin();
                     it != kids->entities.rend(); ++it)
                {
                    stack.push_back(*it);
                }
            }
        }
        return 0u;
    }

    void RegisterLuaBTBindings(sol::state& lua)
    {
        lua.new_usertype<LuaCtxRef>("BTContext",
            sol::no_constructor,
            "GetBB",                   &Ctx_GetBB,
            "SetBB",                   &Ctx_SetBB,
            "HasBB",                   &Ctx_HasBB,
            "RemoveBB",                &Ctx_RemoveBB,
            "Entity",                  &Ctx_Entity,
            "DeltaTime",               &Ctx_DeltaTime,
            "Elapsed",                 &Ctx_Elapsed,
            "GetEntityPosition",       &Ctx_GetEntityPosition,
            "DistanceTo",              &Ctx_DistanceTo,
            "FindAnimatedDescendant",  &Ctx_FindAnimatedDescendant);

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
