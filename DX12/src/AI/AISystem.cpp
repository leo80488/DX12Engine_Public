#include "AI/AISystem.h"
#include "AI/AIComponents.h"
#include "AI/BTAsset.h"
#include "AI/BTNode.h"
#include "AI/BTNodes.h"
#include "ECS/ECS.h"
#include "System/Log.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <fstream>
#include <sstream>

namespace AI
{
    AISystem::AISystem() = default;
    AISystem::~AISystem() = default;

    void AISystem::Init(sol::state* lua)
    {
        m_lua = lua;
        m_actions.BindLua(lua);
    }

    // -----------------------------------------------------------------------
    // Lua-table → C++ node tree conversion
    // -----------------------------------------------------------------------
    // Schema (matches BT_AI_System_Architecture.md §4.1):
    //   {
    //     type     = "Sequence" | "Selector" | "Parallel" |
    //                "Inverter" | "Repeater" | "Cooldown" |
    //                "BlackboardCondition" |
    //                "Action" | "Condition",
    //     name     = "optional designer label",
    //     children = { <subtable>, <subtable>, ... },
    //     -- type-specific fields:
    //     func     = "RegistryName"               -- Action / Condition
    //     params   = { key = value, ... }         -- Action / Condition
    //     count    = N                            -- Repeater
    //     seconds  = X                            -- Cooldown
    //     succeed_on = N                          -- Parallel
    //     fail_on    = N                          -- Parallel
    //     key      = "BBKey"                      -- BlackboardCondition
    //     op       = "is_set" | "equals" | "not_equals"
    //     expected = <BBValue>                    -- BlackboardCondition
    //   }
    //
    // The schema is pragmatic, not over-engineered. New node types can be
    // added by extending ParseNode below; designers should be told the
    // valid `type` strings via a one-page cheatsheet, not a JSON schema.

    static BBValue LuaToBBValue(const sol::object& v)
    {
        if (v.is<bool>())                return v.as<bool>();
        if (v.is<int>())                 return v.as<int>();
        if (v.is<float>())               return v.as<float>();
        if (v.is<std::string>())         return v.as<std::string>();
        if (v.is<sol::table>())
        {
            sol::table t = v.as<sol::table>();
            // Disambiguate scalar XMFLOAT3 from list-of-XMFLOAT3 / list-of-
            // string. The decision is by the first positional element's type:
            //   table  → list of XMFLOAT3 (waypoints, formation slots, …)
            //   string → list of strings  (SetRandomState's candidates, …)
            //   number / nil → scalar XMFLOAT3 path
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
                    // Accept both {x=,y=,z=} and positional {x,y,z}.
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
            // Scalar XMFLOAT3 path — also accept named {x=,y=,z=}.
            DirectX::XMFLOAT3 f3{};
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

    static std::unordered_map<std::string, BBValue>
    ParseParams(const sol::table& t)
    {
        std::unordered_map<std::string, BBValue> out;
        sol::object paramsObj = t["params"];
        if (!paramsObj.is<sol::table>()) return out;
        sol::table params = paramsObj.as<sol::table>();
        for (auto& kv : params)
        {
            if (!kv.first.is<std::string>()) continue;
            out[kv.first.as<std::string>()] = LuaToBBValue(kv.second);
        }
        return out;
    }

    static std::unique_ptr<BTNode> ParseNode(const sol::table& t);

    static void ParseChildren(const sol::table& t, BTNode& parent)
    {
        sol::object kids = t["children"];
        if (!kids.is<sol::table>()) return;
        sol::table children = kids.as<sol::table>();
        for (size_t i = 1; ; ++i)
        {
            sol::object slot = children[i];
            if (!slot.is<sol::table>()) break;
            auto child = ParseNode(slot.as<sol::table>());
            if (child) parent.children.push_back(std::move(child));
        }
    }

    static std::unique_ptr<BTNode> ParseNode(const sol::table& t)
    {
        const std::string type = t.get_or<std::string>("type", "");
        const std::string name = t.get_or<std::string>("name", "");

        std::unique_ptr<BTNode> node;

        if      (type == "Sequence")            node = std::make_unique<Sequence>();
        else if (type == "Selector")            node = std::make_unique<Selector>();
        else if (type == "Parallel")
        {
            auto p = std::make_unique<Parallel>();
            p->succeedOn = t.get_or("succeed_on", 0u);
            p->failOn    = t.get_or("fail_on",    1u);
            node = std::move(p);
        }
        else if (type == "Inverter")            node = std::make_unique<Inverter>();
        else if (type == "Repeater")
        {
            auto r = std::make_unique<Repeater>();
            r->count = t.get_or("count", 1u);
            node = std::move(r);
        }
        else if (type == "Cooldown")
        {
            auto c = std::make_unique<Cooldown>();
            c->seconds = t.get_or("seconds", 1.0f);
            node = std::move(c);
        }
        else if (type == "BlackboardCondition")
        {
            auto b = std::make_unique<BlackboardCondition>();
            b->key = t.get_or<std::string>("key", "");
            const std::string op = t.get_or<std::string>("op", "is_set");
            if      (op == "is_set")     b->op = BBCmpOp::IsSet;
            else if (op == "equals")     b->op = BBCmpOp::Equals;
            else if (op == "not_equals") b->op = BBCmpOp::NotEquals;
            sol::object exp = t["expected"];
            if (!exp.is<sol::nil_t>()) b->expected = LuaToBBValue(exp);
            node = std::move(b);
        }
        else if (type == "Action")
        {
            auto a = std::make_unique<ActionLeaf>();
            a->actionName = t.get_or<std::string>("func", "");
            a->params     = ParseParams(t);
            node = std::move(a);
        }
        else if (type == "Condition")
        {
            auto c = std::make_unique<ConditionLeaf>();
            c->conditionName = t.get_or<std::string>("func", "");
            c->params        = ParseParams(t);
            node = std::move(c);
        }
        else
        {
            LOG_ERROR("BT parse: unknown node type '%s'", type.c_str());
            return nullptr;
        }

        node->name = name;
        ParseChildren(t, *node);
        return node;
    }

    std::shared_ptr<BTAsset> AISystem::ParseFromFile(const std::string& path)
    {
        if (!m_lua)
        {
            LOG_ERROR("BT load: no Lua state bound (Init not called?)");
            return nullptr;
        }

        std::ifstream f(path);
        if (!f.good())
        {
            LOG_ERROR("BT load: cannot open '%s'", path.c_str());
            return nullptr;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string source = ss.str();

        sol::protected_function_result result = m_lua->safe_script(source,
            sol::script_pass_on_error,
            path);
        if (!result.valid())
        {
            sol::error err = result;
            LOG_ERROR("BT load: '%s' lua error: %s", path.c_str(), err.what());
            return nullptr;
        }
        if (result.return_count() == 0 || !result.get<sol::object>().is<sol::table>())
        {
            LOG_ERROR("BT load: '%s' did not return a table", path.c_str());
            return nullptr;
        }

        sol::table root = result.get<sol::table>();
        auto rootNode   = ParseNode(root);
        if (!rootNode) return nullptr;

        auto asset = std::make_shared<BTAsset>();
        asset->root       = std::move(rootNode);
        asset->sourcePath = path;
        asset->AssignIds();

        try {
            m_fileTimestamps[path] = std::filesystem::last_write_time(path);
        } catch (...) {}

        return asset;
    }

    std::shared_ptr<BTAsset> AISystem::AcquireTree(const std::string& path)
    {
        auto it = m_loaded.find(path);
        if (it != m_loaded.end()) return it->second;

        auto asset = ParseFromFile(path);
        if (!asset) return nullptr;
        m_loaded[path] = asset;
        return asset;
    }

    std::shared_ptr<BTAsset> AISystem::ReloadTree(const std::string& path)
    {
        auto fresh = ParseFromFile(path);
        if (!fresh) return nullptr;
        m_loaded[path] = fresh;
        return fresh;
    }

    void AISystem::Update(World& world, float dt)
    {
        m_elapsed += dt;

        auto* pool = world.GetPool<AIComponent>();
        if (!pool) return;

        const auto& ents = pool->Entities();
        for (size_t i = 0; i < ents.size(); ++i)
            TickEntity(world, ents[i], dt);
    }

    void AISystem::TickEntity(World& world, Entity e, float dt)
    {
        AIComponent* ai = world.GetComponent<AIComponent>(e);
        if (!ai || !ai->enabled) return;

        // Deserialised AIComponents arrive with treePath set but tree/instance
        // null — the path is the persistable handle, the parsed BTAsset isn't.
        // Resolve here on first tick so the user doesn't have to re-drop the
        // .bt.lua in the Inspector after every world load.
        if (!ai->tree && !ai->treePath.empty())
        {
            ai->tree = AcquireTree(ai->treePath);
            if (ai->tree && !ai->instance)
                ai->instance = std::make_unique<BTInstance>();
        }
        if (!ai->tree || !ai->tree->root) return;

        ai->timeSinceLastTick += dt;
        if (ai->timeSinceLastTick < ai->tickInterval) return;

        const float tickDt    = ai->timeSinceLastTick;
        ai->timeSinceLastTick = 0.f;

        if (!ai->instance) ai->instance = std::make_unique<BTInstance>();
        if (ai->instance->resetRequested)
        {
            ai->instance->cooldowns.clear();
            ai->instance->repeats.clear();
            ai->instance->runningNode    = kInvalidNodeId;
            ai->instance->resetRequested = false;
        }
        ai->instance->trace.clear();

        BlackboardComponent* bb = world.GetComponent<BlackboardComponent>(e);
        if (!bb)
        {
            world.AddComponent<BlackboardComponent>(e, BlackboardComponent{});
            bb = world.GetComponent<BlackboardComponent>(e);
        }

        BTContext ctx;
        ctx.entity     = e;
        ctx.world      = &world;
        ctx.blackboard = bb;
        ctx.instance   = ai->instance.get();
        ctx.actions    = &m_actions;
        ctx.deltaTime  = tickDt;
        ctx.elapsed    = m_elapsed;

        ai->tree->root->Tick(ctx);
    }

    void AISystem::CheckHotReload(World& world)
    {
        for (auto& [path, oldTime] : m_fileTimestamps)
        {
            try {
                auto newTime = std::filesystem::last_write_time(path);
                if (newTime == oldTime) continue;
                oldTime = newTime;

                auto fresh = ParseFromFile(path);
                if (!fresh) continue;
                m_loaded[path] = fresh;
                OnTreeReloaded(world, path);
                LOG_INFO("AISystem: hot-reload '%s'", path.c_str());
            } catch (...) {}
        }
    }

    void AISystem::OnTreeReloaded(World& world, const std::string& path)
    {
        auto it = m_loaded.find(path);
        if (it == m_loaded.end()) return;

        std::shared_ptr<BTAsset> newTree = it->second;
        auto* pool = world.GetPool<AIComponent>();
        if (!pool) return;
        const auto& ents = pool->Entities();
        for (size_t i = 0; i < ents.size(); ++i)
        {
            AIComponent& ai = pool->Data()[i];
            if (ai.treePath == path)
            {
                ai.tree = newTree;
                if (ai.instance) ai.instance->resetRequested = true;
            }
        }
    }
}
