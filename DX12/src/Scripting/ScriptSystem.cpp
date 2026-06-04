#include "Scripting/ScriptSystem.h"
#include "Scripting/ScriptComponent.h"
#include "Scripting/LuaMathTypes.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/CommandAPI.h"
#include "ECS/TagComponent.h"
#include "ECS/FollowEvents.h"
#include "ECS/EquipmentEvents.h"
#include "Physics/PhysicsEvents.h"
#include "Input/InputSystem.h"
#include "System/EventBus.h"
#include "System/Log.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <algorithm>

using namespace DirectX;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Per-entity Lua instance storage helpers.
//
// Each entity's logic instances live as a Lua array at
// (*m_lua)["__logic_instances"][entityId]. Slots are stored 1-based
// ([slot + 1]) so they read back as a clean Lua array. These helpers keep the
// nested lookup in one place; callers guard on the returned table's .valid().
// ---------------------------------------------------------------------------
namespace
{
    // Returns the per-entity instance array, creating it on demand when
    // `create` is set. When `create` is false and none exists, returns an
    // invalid (nil) table.
    sol::table GetInstanceArray(sol::state& lua, uint32_t e, bool create)
    {
        sol::object o = lua["__logic_instances"][e];
        if (o.get_type() == sol::type::table) return o.as<sol::table>();
        if (!create) return sol::table{};
        sol::table t = lua.create_table();
        lua["__logic_instances"][e] = t;
        return t;
    }

    // Returns the slot's instance table, or an invalid (nil) table if the
    // entity has no array or the slot is empty.
    sol::table GetInstance(sol::state& lua, uint32_t e, std::size_t slot)
    {
        sol::object arr = lua["__logic_instances"][e];
        if (arr.get_type() != sol::type::table) return sol::table{};
        sol::object inst = arr.as<sol::table>()[slot + 1];
        if (inst.get_type() != sol::type::table) return sol::table{};
        return inst.as<sol::table>();
    }
}

// ===========================================================================
// LuaBus (pImpl) — string-keyed Lua event queue.
//
// Split out so the header stays free of sol:: types. Subscribers are marked
// inactive on Unsubscribe (tombstone) and compacted opportunistically, same
// pattern as the C++ EventBus.
// ---------------------------------------------------------------------------
struct ScriptSystem::LuaBus
{
    struct Sub
    {
        std::uint64_t             id;
        std::string               name;
        sol::protected_function   fn;
        bool                      active;
    };

    struct Pending
    {
        std::string   name;
        sol::table    payload;
    };

    std::vector<Sub>     subs;
    std::vector<Pending> pending;
    std::uint64_t        nextId = 0;

    std::uint64_t Subscribe(std::string name, sol::protected_function fn)
    {
        const std::uint64_t id = ++nextId;
        subs.push_back({ id, std::move(name), std::move(fn), true });
        return id;
    }

    bool Unsubscribe(std::uint64_t id)
    {
        for (auto& s : subs)
            if (s.id == id && s.active) { s.active = false; return true; }
        return false;
    }

    void Publish(std::string name, sol::table payload)
    {
        pending.push_back({ std::move(name), std::move(payload) });
    }

    void Dispatch(sol::state& /*lua*/)
    {
        std::vector<Pending> processing;
        std::swap(processing, pending);

        for (const Pending& ev : processing)
        {
            for (const Sub& s : subs)
            {
                if (!s.active || s.name != ev.name) continue;
                auto res = s.fn(ev.payload);
                if (!res.valid())
                {
                    sol::error err = res;
                    LOG_ERROR("Lua event handler '%s' error: %s",
                              ev.name.c_str(), err.what());
                }
            }
        }

        std::size_t inactive = 0;
        for (const Sub& s : subs) if (!s.active) ++inactive;
        if (inactive * 2 > subs.size() && !subs.empty())
        {
            std::vector<Sub> live;
            live.reserve(subs.size() - inactive);
            for (Sub& s : subs) if (s.active) live.push_back(std::move(s));
            subs = std::move(live);
        }
    }
};

// ---------------------------------------------------------------------------
// TimerQueue (pImpl) — `Engine.AfterDelay(seconds, fn)` storage.
//
// Ticks with REAL (unscaled) wall-clock dt so that hit-stop timers expire even
// while ScriptSystem::m_timeScale is near zero. Compaction only when many
// expired entries pile up (matches LuaBus subs pattern).
// ---------------------------------------------------------------------------
struct ScriptSystem::TimerQueue
{
    struct Entry
    {
        std::uint64_t           id;
        float                   remaining;     // seconds to next fire (real-time)
        sol::protected_function fn;
        bool                    active;
    };

    std::vector<Entry> entries;
    std::uint64_t      nextId = 0;

    std::uint64_t Schedule(float seconds, sol::protected_function fn)
    {
        const std::uint64_t id = ++nextId;
        entries.push_back({ id, seconds, std::move(fn), true });
        return id;
    }

    bool Cancel(std::uint64_t id)
    {
        for (auto& e : entries)
            if (e.id == id && e.active) { e.active = false; return true; }
        return false;
    }
};

ScriptSystem::ScriptSystem()
    : m_timers (std::make_unique<TimerQueue>())
    , m_luaBus (std::make_unique<LuaBus>())
{}

ScriptSystem::~ScriptSystem()
{
    // Drop the World listener first — it captures `this`, so a DestroyEntity
    // on the still-alive World after ScriptSystem is gone would crash.
    UnbindWorld();

    // Fire OnShutdown on each system before tearing down state.
    for (auto& sys : m_systems) TeardownSystem(sys);
    m_systems.clear();

    // Unsubscribe our bridge lambdas BEFORE the Lua state goes away — they
    // capture `this` and m_lua, so a post-destruction dispatch would blow up.
    for (auto& unsub : m_cppBridgeUnsubscribers) unsub();
    m_cppBridgeUnsubscribers.clear();

    // Drop every container that may hold sol::function / sol::object refs
    // BEFORE deleting m_lua — otherwise their destructors run *after*
    // `delete m_lua` (members destruct in reverse declaration order) and
    // call luaL_unref on a freed lua_State.
    ClearAll();

    delete m_lua;
}

void ScriptSystem::ClearAll()
{
    m_states.clear();
    m_logicTemplates.clear();
    m_services.clear();
    m_uiScripts.clear();
    // m_systems intentionally NOT cleared here — destructor calls TeardownSystem
    // first (needs OnShutdown), then clears. Live ClearAll callers that want
    // shutdown semantics should iterate TeardownSystem manually.
    if (m_luaBus) { m_luaBus->subs.clear(); m_luaBus->pending.clear(); }
    if (m_timers) m_timers->entries.clear();
    m_timeScale = 1.f;
    // Don't destroy m_lua — bindings stay valid.
}

// ---------------------------------------------------------------------------
// TickTimers — drive AfterDelay callbacks with real (unscaled) dt.
//
// Iteration uses an index-based loop instead of a range-for because a fired
// callback can call Engine.AfterDelay again, which push_backs into the same
// vector and would invalidate iterators.
// ---------------------------------------------------------------------------
void ScriptSystem::TickTimers(float realDt)
{
    if (!m_timers || m_timers->entries.empty()) return;

    const std::size_t count = m_timers->entries.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        auto& e = m_timers->entries[i];
        if (!e.active) continue;
        e.remaining -= realDt;
        if (e.remaining > 0.f) continue;

        e.active = false;
        auto res = e.fn();
        if (!res.valid())
        {
            sol::error err = res;
            LOG_ERROR("Lua AfterDelay callback error: %s", err.what());
        }
    }

    std::size_t dead = 0;
    for (const auto& e : m_timers->entries) if (!e.active) ++dead;
    if (dead * 2 > m_timers->entries.size() && !m_timers->entries.empty())
    {
        std::vector<TimerQueue::Entry> live;
        live.reserve(m_timers->entries.size() - dead);
        for (auto& e : m_timers->entries) if (e.active) live.push_back(std::move(e));
        m_timers->entries = std::move(live);
    }
}

// ===========================================================================
void ScriptSystem::Initialize()
{
    m_lua = new sol::state();
    m_lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                          sol::lib::table, sol::lib::io);
    RegisterBindings();
    RegisterCppEventBridges();
    LOG_SUCCESS("ScriptSystem: Lua %s initialized", LUA_RELEASE);
}

// ===========================================================================
void ScriptSystem::RegisterBindings()
{
    auto& lua = *m_lua;

    // ---- Vec3 ----
    lua.new_usertype<LuaVec3>("Vec3",
        sol::constructors<LuaVec3(), LuaVec3(float,float,float)>(),
        "x", &LuaVec3::x,
        "y", &LuaVec3::y,
        "z", &LuaVec3::z,
        "Length",     &LuaVec3::Length,
        "Normalized", &LuaVec3::Normalized,
        sol::meta_function::addition,       &LuaVec3::operator+,
        sol::meta_function::subtraction,    &LuaVec3::operator-,
        sol::meta_function::multiplication, &LuaVec3::operator*,
        sol::meta_function::to_string, [](const LuaVec3& v) {
            char buf[64]; snprintf(buf, sizeof(buf), "Vec3(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
            return std::string(buf);
        }
    );

    // ---- Quat ----
    lua.new_usertype<LuaQuat>("Quat",
        sol::constructors<LuaQuat(), LuaQuat(float,float,float,float)>(),
        "x", &LuaQuat::x, "y", &LuaQuat::y, "z", &LuaQuat::z, "w", &LuaQuat::w
    );

    // ---- LocalTransform ----
    lua.new_usertype<LocalTransform>("LocalTransform",
        "translation", sol::property(
            [](LocalTransform& lt) -> LuaVec3 { return {lt.translation.x, lt.translation.y, lt.translation.z}; },
            [](LocalTransform& lt, LuaVec3 v) { lt.translation = {v.x, v.y, v.z}; }),
        "scale", sol::property(
            [](LocalTransform& lt) -> LuaVec3 { return {lt.scale.x, lt.scale.y, lt.scale.z}; },
            [](LocalTransform& lt, LuaVec3 v) { lt.scale = {v.x, v.y, v.z}; }),
        "rotation", sol::property(
            [](LocalTransform& lt) -> LuaQuat { return {lt.rotation.x, lt.rotation.y, lt.rotation.z, lt.rotation.w}; },
            [](LocalTransform& lt, LuaQuat q) { lt.rotation = {q.x, q.y, q.z, q.w}; })
    );

    // ---- LightData ----
    lua.new_usertype<LightData>("Light",
        "radius",      &LightData::radius,
        "intensity",   &LightData::intensity,
        "spotAngle",   &LightData::spotAngle,
        "castsShadow", &LightData::castsShadow,
        "type", sol::property(
            [](LightData& l) -> int  { return static_cast<int>(l.type); },
            [](LightData& l, int v)  { l.type = static_cast<LightType>(v); }),
        "color", sol::property(
            [](LightData& l) -> LuaVec3 { return {l.color.x, l.color.y, l.color.z}; },
            [](LightData& l, LuaVec3 v) { l.color = {v.x, v.y, v.z}; }),
        "direction", sol::property(
            [](LightData& l) -> LuaVec3 { return {l.direction.x, l.direction.y, l.direction.z}; },
            [](LightData& l, LuaVec3 v) { l.direction = {v.x, v.y, v.z}; })
    );

    lua["LightType"] = lua.create_table_with(
        "Directional", 0, "Point", 1, "Spot", 2);

    // ---- CameraComponent (lens / view-projection) ----
    // The camera pose is on LocalTransform — move the camera via
    // GetLocalTransform(id).position / .rotation, not through this usertype.
    lua.new_usertype<CameraComponent>("Camera",
        "fov",   &CameraComponent::fov,
        "nearZ", &CameraComponent::nearZ,
        "farZ",  &CameraComponent::farZ
    );

    // ---- CameraControllerComponent (FPS controller state) ----
    lua.new_usertype<CameraControllerComponent>("CameraController",
        "yaw",              &CameraControllerComponent::yaw,
        "pitch",            &CameraControllerComponent::pitch,
        "mouseSensitivity", &CameraControllerComponent::mouseSensitivity,
        "moveSpeed",        &CameraControllerComponent::moveSpeed
    );

    // ---- AnimationComponent ----
    lua.new_usertype<AnimationComponent>("Animation",
        "primaryClip",   &AnimationComponent::primaryClip,
        "secondaryClip", &AnimationComponent::secondaryClip,
        "primaryTime",   &AnimationComponent::primaryTime,
        "secondaryTime", &AnimationComponent::secondaryTime,
        "blendWeight",   &AnimationComponent::blendWeight,
        "speed",         &AnimationComponent::speed,
        "looping",       &AnimationComponent::looping,
        "paused",        &AnimationComponent::paused
    );

    // ---- Input ----
    // Routes through the shared Input snapshot so Lua sees the same edge
    // state as native systems within a frame. Mouse buttons go through the
    // same VK_*BUTTON path because we don't have a separate per-frame mouse
    // button snapshot yet — Mouse.h is event-driven, which doesn't give us
    // edge queries cheap. WasKeyPressed / WasKeyReleased are exposed too so
    // Lua Logic scripts can detect single-frame transitions without manual
    // edge tracking.
    auto input = lua.create_table();
    input.set_function("IsKeyDown", [](int key) -> bool {
        return Input::Get().IsKeyDown(key);
    });
    input.set_function("WasKeyPressed", [](int key) -> bool {
        return Input::Get().WasKeyPressed(key);
    });
    input.set_function("WasKeyReleased", [](int key) -> bool {
        return Input::Get().WasKeyReleased(key);
    });
    input.set_function("IsMouseDown", [](int button) -> bool {
        int vk = (button == 1) ? VK_RBUTTON : (button == 2) ? VK_MBUTTON : VK_LBUTTON;
        return Input::Get().IsKeyDown(vk);
    });
    input.set_function("GetMousePos", []() -> std::tuple<float,float> {
        POINT p; GetCursorPos(&p);
        if (HWND hwnd = GetForegroundWindow())
            ScreenToClient(hwnd, &p);
        return { static_cast<float>(p.x), static_cast<float>(p.y) };
    });
    lua["Input"] = input;

    lua["Mouse"] = lua.create_table_with("Left", 0, "Right", 1, "Middle", 2);

    auto keys = lua.create_table();
    keys["W"] = 0x57; keys["A"] = 0x41; keys["S"] = 0x53; keys["D"] = 0x44;
    keys["Q"] = 0x51; keys["E"] = 0x45;
    keys["Up"] = VK_UP; keys["Down"] = VK_DOWN;
    keys["Left"] = VK_LEFT; keys["Right"] = VK_RIGHT;
    keys["Space"] = VK_SPACE; keys["Shift"] = VK_SHIFT;
    keys["Ctrl"] = VK_CONTROL;
    lua["Keys"] = keys;

    // ---- Log ----
    auto log = lua.create_table();
    log.set_function("Info",    [](const std::string& msg) { LOG_INFO("Lua: %s", msg.c_str()); });
    log.set_function("Warning", [](const std::string& msg) { LOG_WARNING("Lua: %s", msg.c_str()); });
    log.set_function("Error",   [](const std::string& msg) { LOG_ERROR("Lua: %s", msg.c_str()); });
    lua["Log"] = log;

    // ---- Time (updated each frame in Update) ----
    lua["Time"] = lua.create_table_with("dt", 0.f, "elapsed", 0.f);

    // ---- Engine.* Command API ------------------------------------------------
    auto engine = lua.create_table();

    // ---- Equipment / Follow Commands ----
    engine.set_function("EquipToSocket",
        [this](uint32_t charId, uint32_t itemId, const std::string& socketName) -> bool
        {
            if (!m_world) { LOG_WARNING("Engine.EquipToSocket called outside Update"); return false; }
            return Command::EquipToSocket(*m_world,
                static_cast<Entity>(charId),
                static_cast<Entity>(itemId),
                socketName.c_str());
        });

    engine.set_function("UnequipItem",
        [this](uint32_t itemId) -> bool
        {
            if (!m_world) { LOG_WARNING("Engine.UnequipItem called outside Update"); return false; }
            return Command::UnequipItem(*m_world, static_cast<Entity>(itemId));
        });

    engine.set_function("AttachToEntity",
        [this](uint32_t followerId, uint32_t targetId) -> bool
        {
            if (!m_world) { LOG_WARNING("Engine.AttachToEntity called outside Update"); return false; }
            return Command::AttachToEntity(*m_world,
                static_cast<Entity>(followerId),
                static_cast<Entity>(targetId));
        });

    engine.set_function("DetachFromEntity",
        [this](uint32_t followerId) -> bool
        {
            if (!m_world) { LOG_WARNING("Engine.DetachFromEntity called outside Update"); return false; }
            return Command::DetachFromEntity(*m_world, static_cast<Entity>(followerId));
        });

    // ---- Entity lifecycle ---------------------------------------------------
    engine.set_function("CreateEntity",
        [this]() -> uint32_t
        {
            if (!m_world) { LOG_WARNING("Engine.CreateEntity called outside Update"); return 0u; }
            return static_cast<uint32_t>(m_world->CreateEntity());
        });

    engine.set_function("DestroyEntity",
        [this](uint32_t id)
        {
            if (!m_world) { LOG_WARNING("Engine.DestroyEntity called outside Update"); return; }
            if (id == NullEntity) return;
            m_world->DestroyEntity(static_cast<Entity>(id));
        });

    engine.set_function("IsAlive",
        [this](uint32_t id) -> bool
        {
            if (!m_world) return false;
            return m_world->IsAlive(static_cast<Entity>(id));
        });

    engine.set_function("SetName",
        [this](uint32_t id, const std::string& name)
        {
            if (!m_world) return;
            m_world->SetName(static_cast<Entity>(id), name);
        });

    engine.set_function("GetName",
        [this](uint32_t id) -> std::string
        {
            if (!m_world) return {};
            return m_world->GetName(static_cast<Entity>(id));
        });

    engine.set_function("AttachScript",
        [this](uint32_t id, const std::string& path) -> bool
        {
            if (!m_world) return false;
            const Entity e = static_cast<Entity>(id);
            if (!m_world->IsAlive(e)) return false;

            // Append a slot — never clobber scripts already attached. The new
            // slot is picked up (template loaded, OnSpawn fired) on the next
            // ScriptSystem::Update when the reconcile loop sees the longer
            // ScriptComponent::scripts vector.
            ScriptComponent* sc = m_world->GetComponent<ScriptComponent>(e);
            if (!sc)
            {
                m_world->AddComponent<ScriptComponent>(e, {});
                sc = m_world->GetComponent<ScriptComponent>(e);
            }
            if (!sc) return false;

            ScriptInstance si;
            si.scriptPath = path;
            si.enabled    = true;
            sc->scripts.push_back(std::move(si));
            return true;
        });

    // ---- Component access ---------------------------------------------------
    engine.set_function("GetLocalTransform",
        [this](uint32_t id) -> LocalTransform*
        {
            if (!m_world) return nullptr;
            return m_world->GetComponent<LocalTransform>(static_cast<Entity>(id));
        });

    engine.set_function("GetLight",
        [this](uint32_t id) -> LightData*
        {
            if (!m_world) return nullptr;
            return m_world->GetComponent<LightData>(static_cast<Entity>(id));
        });

    engine.set_function("GetCamera",
        [this](uint32_t id) -> CameraComponent*
        {
            if (!m_world) return nullptr;
            return m_world->GetComponent<CameraComponent>(static_cast<Entity>(id));
        });

    engine.set_function("GetCameraController",
        [this](uint32_t id) -> CameraControllerComponent*
        {
            if (!m_world) return nullptr;
            return m_world->GetComponent<CameraControllerComponent>(static_cast<Entity>(id));
        });

    engine.set_function("GetAnimation",
        [this](uint32_t id) -> AnimationComponent*
        {
            if (!m_world) return nullptr;
            return m_world->GetComponent<AnimationComponent>(static_cast<Entity>(id));
        });

    // BFS the hierarchy under `id` and return the first descendant that owns
    // an AnimationComponent (or `id` itself if it qualifies). Returns 0 when
    // nothing in the subtree is animated.
    //
    // Use case: a character split as
    //   root  (collider / rigidbody / AI / script)
    //     └── meshNode (skinned mesh + AnimationComponent)
    // — the BT runs on root but FSM / clip transitions need to be issued
    // against the meshNode. Logic scripts call this once on OnSpawn and
    // cache the result.
    engine.set_function("FindAnimatedDescendant",
        [this](uint32_t id) -> uint32_t
        {
            if (!m_world) return 0u;
            const Entity start = static_cast<Entity>(id);
            if (!m_world->IsAlive(start)) return 0u;

            std::vector<Entity> stack;
            stack.push_back(start);
            while (!stack.empty())
            {
                const Entity e = stack.back();
                stack.pop_back();
                if (m_world->GetComponent<AnimationComponent>(e))
                    return static_cast<uint32_t>(e);
                if (const auto* kids = m_world->GetComponent<Children>(e))
                {
                    // Reverse-push to preserve a depth-first walk that
                    // visits earlier children first.
                    for (auto it = kids->entities.rbegin();
                         it != kids->entities.rend(); ++it)
                    {
                        stack.push_back(*it);
                    }
                }
            }
            return 0u;
        });

    engine.set_function("HasComponent",
        [this](uint32_t id, const std::string& name) -> bool
        {
            if (!m_world) return false;
            const Entity e = static_cast<Entity>(id);
            if      (name == "LocalTransform")    return m_world->HasComponent<LocalTransform>(e);
            else if (name == "Light")             return m_world->HasComponent<LightData>(e);
            else if (name == "Camera")            return m_world->HasComponent<CameraComponent>(e);
            else if (name == "CameraController")  return m_world->HasComponent<CameraControllerComponent>(e);
            else if (name == "Animation")         return m_world->HasComponent<AnimationComponent>(e);
            else if (name == "Script")            return m_world->HasComponent<ScriptComponent>(e);
            else if (name == "Tag")               return m_world->HasComponent<TagComponent>(e);
            LOG_WARNING("Engine.HasComponent: unknown component name '%s'", name.c_str());
            return false;
        });

    // ---- Tags ---------------------------------------------------------------
    engine.set_function("AddTag",
        [this](uint32_t id, const std::string& tag) -> bool
        {
            if (!m_world || tag.empty()) return false;
            const Entity e = static_cast<Entity>(id);
            if (!m_world->IsAlive(e)) return false;
            TagComponent* tc = m_world->GetComponent<TagComponent>(e);
            if (!tc) { m_world->AddComponent<TagComponent>(e, {}); tc = m_world->GetComponent<TagComponent>(e); }
            return tc && tc->Add(tag.c_str());
        });

    engine.set_function("HasTag",
        [this](uint32_t id, const std::string& tag) -> bool
        {
            if (!m_world) return false;
            const TagComponent* tc = m_world->GetComponent<TagComponent>(static_cast<Entity>(id));
            return tc && tc->Has(tag.c_str());
        });

    engine.set_function("RemoveTag",
        [this](uint32_t id, const std::string& tag) -> bool
        {
            if (!m_world) return false;
            TagComponent* tc = m_world->GetComponent<TagComponent>(static_cast<Entity>(id));
            return tc && tc->Remove(tag.c_str());
        });

    // ---- Game time scale ----------------------------------------------------
    engine.set_function("SetTimeScale",
        [this](float s) { SetTimeScale(s); });
    engine.set_function("GetTimeScale",
        [this]() -> float { return GetTimeScale(); });

    // ---- AfterDelay ---------------------------------------------------------
    engine.set_function("AfterDelay",
        [this](float seconds, sol::protected_function fn) -> uint64_t
        {
            if (!fn.valid())
            { LOG_WARNING("Engine.AfterDelay: invalid callback"); return 0ull; }
            if (seconds < 0.f) seconds = 0.f;
            return m_timers->Schedule(seconds, std::move(fn));
        });

    engine.set_function("CancelDelay",
        [this](uint64_t id) -> bool { return m_timers->Cancel(id); });

    // ---- Lua event bus ------------------------------------------------------
    engine.set_function("Subscribe",
        [this](const std::string& name, sol::protected_function fn) -> uint64_t
        {
            if (!fn.valid()) { LOG_WARNING("Engine.Subscribe: invalid callback"); return 0ull; }
            return m_luaBus->Subscribe(name, std::move(fn));
        });

    engine.set_function("Unsubscribe",
        [this](uint64_t id) -> bool
        {
            return m_luaBus->Unsubscribe(id);
        });

    engine.set_function("Publish",
        [this](const std::string& name, sol::object payload)
        {
            sol::table t;
            if (payload.is<sol::table>())
                t = payload.as<sol::table>();
            else
                t = m_lua->create_table();
            m_luaBus->Publish(name, std::move(t));
        });

    // ---- Logic-instance baseline access (entity ID required) ---------------
    // Exposes the per-entity baseline transform captured the first time the
    // script ran. Logic scripts can reach this via:
    //     local base = Engine.GetBasePosition(self.entity)
    engine.set_function("GetBasePosition",
        [this](uint32_t id) -> sol::table
        {
            sol::table t = m_lua->create_table();
            const auto it = m_states.find(static_cast<Entity>(id));
            if (it != m_states.end() && it->second.baseCaptured) {
                const auto& p = it->second.baseTransform.translation;
                t["x"] = p.x; t["y"] = p.y; t["z"] = p.z;
            } else {
                t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f;
            }
            return t;
        });
    engine.set_function("GetBaseRotation",
        [this](uint32_t id) -> sol::table
        {
            sol::table t = m_lua->create_table();
            const auto it = m_states.find(static_cast<Entity>(id));
            if (it != m_states.end() && it->second.baseCaptured) {
                const auto& r = it->second.baseTransform.rotation;
                t["x"] = r.x; t["y"] = r.y; t["z"] = r.z; t["w"] = r.w;
            } else {
                t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f; t["w"] = 1.f;
            }
            return t;
        });
    engine.set_function("GetBaseScale",
        [this](uint32_t id) -> sol::table
        {
            sol::table t = m_lua->create_table();
            const auto it = m_states.find(static_cast<Entity>(id));
            if (it != m_states.end() && it->second.baseCaptured) {
                const auto& s = it->second.baseTransform.scale;
                t["x"] = s.x; t["y"] = s.y; t["z"] = s.z;
            } else {
                t["x"] = 1.f; t["y"] = 1.f; t["z"] = 1.f;
            }
            return t;
        });
    engine.set_function("ResetBase",
        [this](uint32_t id)
        {
            if (!m_world) return;
            auto it = m_states.find(static_cast<Entity>(id));
            if (it == m_states.end()) return;
            if (const LocalTransform* lt =
                m_world->GetComponent<LocalTransform>(static_cast<Entity>(id)))
            {
                it->second.baseTransform = *lt;
                it->second.baseCaptured  = true;
            }
        });

    // ---- System / Service / UI / Config access (doc §8.3) ------------------
    // Returns the registered system/service/ui table, or nil. Lua side typical:
    //     local Damage = Engine.GetService("Damage")
    //     local quest  = Engine.GetSystem("QuestSystem")
    //     local hud    = Engine.GetUIScript("HUD")
    engine.set_function("GetService",
        [this](const std::string& name) -> sol::object
        {
            auto it = m_services.find(name);
            if (it == m_services.end() || !it->second.loaded)
                return sol::lua_nil;
            return (*m_lua)["__services"][name];
        });

    engine.set_function("GetSystem",
        [this](const std::string& name) -> sol::object
        {
            for (const auto& sys : m_systems)
                if (sys.name == name && sys.loaded)
                    return (*m_lua)["__systems"][name];
            return sol::lua_nil;
        });

    engine.set_function("GetUIScript",
        [this](const std::string& name) -> sol::object
        {
            auto it = m_uiScripts.find(name);
            if (it == m_uiScripts.end() || !it->second.loaded)
                return sol::lua_nil;
            return (*m_lua)["__ui_scripts"][name];
        });

    // Pure data loader: runs the file and returns its return value as-is.
    // Caller decides whether to cache.
    engine.set_function("LoadConfig",
        [this](const std::string& path) -> sol::object
        {
            auto result = m_lua->safe_script_file(path, sol::script_pass_on_error);
            if (!result.valid()) {
                sol::error err = result;
                LOG_ERROR("Engine.LoadConfig [%s]: %s", path.c_str(), err.what());
                return sol::lua_nil;
            }
            sol::object ret = result;
            return ret;
        });

    // ---- Trigger / Animation manual fire ------------------------------------
    // Lua-side equivalents to the C++ event bridges. PublishTrigger is for
    // non-physics overlap detection (custom AABB queries, etc.); PublishAnimEvent
    // lets gameplay or future state machines fire OnAnimEvent on a Logic instance.
    engine.set_function("PublishTrigger",
        [this](uint32_t selfId, uint32_t otherId)
        {
            auto sIt = m_states.find(static_cast<Entity>(selfId));
            if (sIt == m_states.end()) return;
            // Fire OnEnter on every slot whose instance defines it. No
            // point/normal for manual triggers — pass nil so scripts can
            // distinguish (or just ignore). Same arg shape as the physics path.
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, selfId, i);
                if (!inst.valid()) continue;
                sol::protected_function fn = inst["OnEnter"];
                if (!fn.valid()) continue;
                auto res = fn(inst, otherId, sol::lua_nil, sol::lua_nil);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnEnter error [%s] entity %u slot %zu: %s",
                              sIt->second.slots[i].path.c_str(), selfId, i, err.what());
                }
            }
        });

    engine.set_function("PublishAnimEvent",
        [this](uint32_t entityId, const std::string& name, sol::object payload)
        {
            auto sIt = m_states.find(static_cast<Entity>(entityId));
            if (sIt == m_states.end()) return;
            // Forward payload as-is (table, number, string, nil — script's
            // contract). Engine doesn't synthesize an empty table here. Fire on
            // every slot whose instance defines OnAnimEvent.
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, entityId, i);
                if (!inst.valid()) continue;
                sol::protected_function fn = inst["OnAnimEvent"];
                if (!fn.valid()) continue;
                auto res = fn(inst, name, payload);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnAnimEvent error [%s] entity %u slot %zu name=%s: %s",
                              sIt->second.slots[i].path.c_str(), entityId, i,
                              name.c_str(), err.what());
                }
            }
        });

    lua["Engine"] = engine;

    // ---- Registry-style storage tables (per script category) ---------------
    // Templates and instances for Logic scripts.
    lua["__logic_templates"] = lua.create_table();
    lua["__logic_instances"] = lua.create_table();
    // Singleton instance tables for systems / services / ui scripts.
    lua["__systems"]         = lua.create_table();
    lua["__services"]        = lua.create_table();
    lua["__ui_scripts"]      = lua.create_table();
}

// ---------------------------------------------------------------------------
// Exposed-variable parsing helpers (file-local). Read a script's `exposed`
// table into the C++ ScriptVarDesc schema. Kept in an anonymous namespace so
// the sol-typed reading logic stays out of the header.
// ---------------------------------------------------------------------------
namespace
{
    // Read a 3-float vector from a Lua value shaped as an array {x,y,z}, a
    // keyed {x=,y=,z=}, or a color {r=,g=,b=}. Returns false if not vec-shaped.
    bool ReadExposedVec3(const sol::object& o, float out[3])
    {
        if (o.get_type() != sol::type::table) return false;
        sol::table t = o.as<sol::table>();
        sol::optional<float> a1 = t[1], a2 = t[2], a3 = t[3];
        if (a1 && a2 && a3) { out[0]=*a1; out[1]=*a2; out[2]=*a3; return true; }
        sol::optional<float> x = t["x"], y = t["y"], z = t["z"];
        if (x && y && z) { out[0]=*x; out[1]=*y; out[2]=*z; return true; }
        sol::optional<float> r = t["r"], g = t["g"], b = t["b"];
        if (r && g && b) { out[0]=*r; out[1]=*g; out[2]=*b; return true; }
        return false;
    }

    // Build a default ScriptVarValue of `type` from a Lua `default` object
    // (which may be nil → a type-appropriate zero/empty default).
    ScriptVarValue ReadExposedDefault(ScriptVarType type, const sol::object& def)
    {
        switch (type)
        {
        case ScriptVarType::Float:
            return ScriptVarValue::MakeFloat(def.is<double>() ? (float)def.as<double>() : 0.f);
        case ScriptVarType::Int:
            return ScriptVarValue::MakeInt(def.is<double>() ? (int32_t)def.as<double>() : 0);
        case ScriptVarType::Bool:
            return ScriptVarValue::MakeBool(def.is<bool>() ? def.as<bool>() : false);
        case ScriptVarType::Float3:
        case ScriptVarType::Color:
        {
            float v[3] = { 0.f, 0.f, 0.f };
            ReadExposedVec3(def, v);
            return type == ScriptVarType::Color
                ? ScriptVarValue::MakeColor (v[0], v[1], v[2])
                : ScriptVarValue::MakeFloat3(v[0], v[1], v[2]);
        }
        case ScriptVarType::Entity:
            return ScriptVarValue::MakeEntity(def.is<double>() ? (uint32_t)def.as<double>() : 0u);
        case ScriptVarType::String:
            return ScriptVarValue::MakeString(def.is<std::string>() ? def.as<std::string>() : std::string());
        case ScriptVarType::Asset:
            return ScriptVarValue::MakeAsset(def.is<std::string>() ? def.as<std::string>() : std::string());
        }
        return ScriptVarValue::MakeFloat(0.f);
    }
}

// ===========================================================================
// LoadLogicTemplate — load a .lua file as a Logic prototype. Expects the file
// to `return T` where T is a table containing OnSpawn/OnUpdate/OnDestroy
// methods (any subset). Subsequent entities sharing this path get instances
// via metatable __index inheritance.
// ---------------------------------------------------------------------------
bool ScriptSystem::LoadLogicTemplate(const std::string& path)
{
    auto& tmpl = m_logicTemplates[path];
    tmpl.path = path;
    tmpl.lastError.clear();
    tmpl.hasSpawn = tmpl.hasUpdate = tmpl.hasDestroy = false;

    auto result = m_lua->safe_script_file(path, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        tmpl.lastError = err.what();
        LOG_ERROR("ScriptSystem: logic load error [%s]: %s", path.c_str(), err.what());
        return false;
    }

    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        tmpl.lastError = "Logic script must `return T` where T is a table.";
        LOG_ERROR("ScriptSystem: logic [%s] did not return a table", path.c_str());
        return false;
    }

    sol::table t = ret.as<sol::table>();
    (*m_lua)["__logic_templates"][path] = t;

    tmpl.hasSpawn   = t["OnSpawn"].valid();
    tmpl.hasUpdate  = t["OnUpdate"].valid();
    tmpl.hasDestroy = t["OnDestroy"].valid();

    // Parse the editor-exposed variable schema (`T.exposed`), if any.
    ParseExposedSchema(path, tmpl.exposed);

    try { m_fileTimestamps[path] = fs::last_write_time(path); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded logic template [%s] (OnSpawn=%d OnUpdate=%d OnDestroy=%d)",
             path.c_str(), tmpl.hasSpawn, tmpl.hasUpdate, tmpl.hasDestroy);
    return true;
}

// ---------------------------------------------------------------------------
// EnsureLogicInstance — create one slot's instance from the template, install
// metatable inheritance, and fire OnSpawn(self, entity).
// ---------------------------------------------------------------------------
bool ScriptSystem::EnsureLogicInstance(Entity e, std::size_t slot, const std::string& path,
                                       const std::unordered_map<std::string, ScriptVarValue>* overrides)
{
    auto tmplIt = m_logicTemplates.find(path);
    if (tmplIt == m_logicTemplates.end()) return false;

    sol::table tmpl = (*m_lua)["__logic_templates"][path];
    if (!tmpl.valid()) return false;

    sol::table inst = m_lua->create_table();
    sol::table mt   = m_lua->create_table();
    mt["__index"]   = tmpl;
    inst[sol::metatable_key] = mt;

    // Convenience: every instance has self.entity and self.slot pre-set so a
    // script can tell which entity it is bound to and which slot it occupies.
    inst["entity"] = static_cast<uint32_t>(e);
    inst["slot"]   = static_cast<uint32_t>(slot);

    sol::table arr = GetInstanceArray(*m_lua, static_cast<uint32_t>(e), /*create*/ true);
    arr[slot + 1] = inst;

    // Inject editor-exposed variables (schema defaults + per-slot overrides)
    // onto the instance table BEFORE OnSpawn so the script reads them as
    // self.<name> on its very first tick.
    InjectExposedVars(e, slot, path, overrides);

    if (tmplIt->second.hasSpawn)
    {
        sol::protected_function fn = tmpl["OnSpawn"];
        auto res = fn(inst, static_cast<uint32_t>(e));
        if (!res.valid()) {
            sol::error err = res;
            LOG_ERROR("Lua OnSpawn error [%s] entity %u slot %zu: %s",
                      path.c_str(), e, slot, err.what());
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// FireSlotDestroy — fire OnDestroy(self) on one slot and drop its Lua table.
//
// Takes the slot metadata BY VALUE-equivalent (copied locally up front) so it
// never touches m_states after the callback — the OnDestroy handler is free to
// destroy entities, which mutates m_states. Crucially it NILS the registry slot
// BEFORE invoking OnDestroy (holding a local ref so `self` stays valid): a
// re-entrant teardown of the same slot — e.g. an OnDestroy that destroys its own
// entity — then sees the slot already gone and cannot double-fire.
// ---------------------------------------------------------------------------
void ScriptSystem::FireSlotDestroy(Entity e, std::size_t slot, const LogicSlot& ls)
{
    const std::string path       = ls.path;        // copy — `ls` may be erased
    const bool        initCalled = ls.initCalled;  // out from under us by OnDestroy

    sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(e), slot);

    // Detach from the registry first so re-entrant teardown is a no-op. `inst`
    // keeps the table alive across the OnDestroy call.
    sol::table arr = GetInstanceArray(*m_lua, static_cast<uint32_t>(e), /*create*/ false);
    if (arr.valid()) arr[slot + 1] = sol::lua_nil;

    if (!inst.valid()) return;

    auto tmplIt = m_logicTemplates.find(path);
    if (tmplIt != m_logicTemplates.end() && tmplIt->second.hasDestroy && initCalled)
    {
        sol::protected_function fn = inst["OnDestroy"];
        if (fn.valid())
        {
            auto res = fn(inst);
            if (!res.valid()) {
                sol::error err = res;
                LOG_ERROR("Lua OnDestroy error [%s] entity %u slot %zu: %s",
                          path.c_str(), e, slot, err.what());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// DestroyLogicInstance — tear down ONE slot's instance while keeping the
// entity's state (used by the Update reconcile on shrink / path change). The
// caller must re-find m_states[e] afterwards: OnDestroy may have erased it.
// ---------------------------------------------------------------------------
void ScriptSystem::DestroyLogicInstance(Entity e, std::size_t slot)
{
    auto sIt = m_states.find(e);
    if (sIt == m_states.end() || slot >= sIt->second.slots.size()) return;
    FireSlotDestroy(e, slot, sIt->second.slots[slot]);
}

// ---------------------------------------------------------------------------
// TeardownEntity — destroy every slot of an entity and drop its whole instance
// array. Used on entity destruction / sweep. Erases the m_states entry FIRST
// (working from a moved-out copy of the slots) so a re-entrant DestroyEntity
// from within an OnDestroy callback finds nothing and cannot double-tear-down.
// ---------------------------------------------------------------------------
void ScriptSystem::TeardownEntity(Entity e)
{
    auto it = m_states.find(e);
    if (it == m_states.end()) return;
    const std::vector<LogicSlot> slots = std::move(it->second.slots);
    m_states.erase(it);
    for (std::size_t i = 0; i < slots.size(); ++i)
        FireSlotDestroy(e, i, slots[i]);
    (*m_lua)["__logic_instances"][static_cast<uint32_t>(e)] = sol::lua_nil;
}

// ---------------------------------------------------------------------------
// ParseExposedSchema — read a template's `exposed` table into ScriptVarDesc.
// Accepts both the explicit-descriptor form ({type='float',default=,min=,...})
// and bare-literal shorthands (a number/bool/string/3-array infers its type).
// Output is sorted by name for a stable inspector layout.
// ---------------------------------------------------------------------------
void ScriptSystem::ParseExposedSchema(const std::string& path, std::vector<ScriptVarDesc>& out)
{
    out.clear();
    if (!m_lua) return;

    sol::table tmpl = (*m_lua)["__logic_templates"][path];
    if (!tmpl.valid()) return;

    sol::object exposedObj = tmpl["exposed"];
    if (exposedObj.get_type() != sol::type::table) return;
    sol::table exposed = exposedObj.as<sol::table>();

    for (auto& kv : exposed)
    {
        if (kv.first.get_type() != sol::type::string) continue;  // skip array part
        const std::string name = kv.first.as<std::string>();
        const sol::object  val  = kv.second;

        ScriptVarDesc d;
        d.name = name;

        switch (val.get_type())
        {
        case sol::type::number:
            d.type   = ScriptVarType::Float;
            d.defVal = ScriptVarValue::MakeFloat((float)val.as<double>());
            break;
        case sol::type::boolean:
            d.type   = ScriptVarType::Bool;
            d.defVal = ScriptVarValue::MakeBool(val.as<bool>());
            break;
        case sol::type::string:
            d.type   = ScriptVarType::String;
            d.defVal = ScriptVarValue::MakeString(val.as<std::string>());
            break;
        case sol::type::table:
        {
            sol::table desc = val.as<sol::table>();
            sol::optional<std::string> typeStr = desc["type"];
            if (typeStr && ScriptVarTypeFromString(*typeStr, d.type))
            {
                d.defVal = ReadExposedDefault(d.type, desc["default"]);
                if (sol::optional<float>       mn  = desc["min"])     { d.minVal = *mn;  d.hasMin = true; }
                if (sol::optional<float>       mx  = desc["max"])     { d.maxVal = *mx;  d.hasMax = true; }
                if (sol::optional<float>       sp  = desc["step"])      d.speed   = *sp;
                if (sol::optional<float>       sp2 = desc["speed"])     d.speed   = *sp2;
                if (sol::optional<std::string> tip = desc["tooltip"])   d.tooltip = *tip;
                if (sol::optional<std::string> lbl = desc["label"])     d.label   = *lbl;
                if (sol::optional<std::string> ext = desc["ext"])       d.assetExt= *ext;
                if (sol::optional<bool>        hdr = desc["hdr"])       d.hdr     = *hdr;
            }
            else
            {
                // Bare 3-number array shorthand → Float3 (no explicit type).
                float v[3] = { 0.f, 0.f, 0.f };
                if (ReadExposedVec3(val, v))
                {
                    d.type   = ScriptVarType::Float3;
                    d.defVal = ScriptVarValue::MakeFloat3(v[0], v[1], v[2]);
                }
                else
                {
                    continue;  // unrecognised table → skip
                }
            }
        } break;
        default:
            continue;  // function / userdata / nil → not an exposed variable
        }

        out.push_back(std::move(d));
    }

    std::sort(out.begin(), out.end(),
        [](const ScriptVarDesc& a, const ScriptVarDesc& b) { return a.name < b.name; });
}

// ---------------------------------------------------------------------------
// GetExposedSchema — lazily load the template so the inspector can show
// variables before the script first ticks; returns its parsed schema.
// ---------------------------------------------------------------------------
const std::vector<ScriptVarDesc>& ScriptSystem::GetExposedSchema(const std::string& path)
{
    static const std::vector<ScriptVarDesc> kEmpty;
    if (path.empty() || !m_lua) return kEmpty;

    auto it = m_logicTemplates.find(path);
    if (it == m_logicTemplates.end())
    {
        if (!LoadLogicTemplate(path)) return kEmpty;
        it = m_logicTemplates.find(path);
        if (it == m_logicTemplates.end()) return kEmpty;
    }
    return it->second.exposed;
}

// ---------------------------------------------------------------------------
// InjectExposedVars — write each schema variable's resolved value (override if
// present, else default) onto the entity's live instance table. Float3/Color
// land as Vec3 userdata (self.v.x/.y/.z); strings/assets as plain strings.
// ---------------------------------------------------------------------------
void ScriptSystem::InjectExposedVars(Entity e, std::size_t slot, const std::string& path,
                                     const std::unordered_map<std::string, ScriptVarValue>* overrides)
{
    auto it = m_logicTemplates.find(path);
    if (it == m_logicTemplates.end()) return;
    const std::vector<ScriptVarDesc>& schema = it->second.exposed;
    if (schema.empty()) return;

    sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(e), slot);
    if (!inst.valid()) return;

    static const std::unordered_map<std::string, ScriptVarValue> kEmpty;
    const std::unordered_map<std::string, ScriptVarValue>& ovr = overrides ? *overrides : kEmpty;

    for (const ScriptVarDesc& d : schema)
    {
        const ScriptVarValue v = ResolveScriptVar(d, ovr);
        switch (v.type)
        {
        case ScriptVarType::Float:  inst[d.name] = (double)v.data.f;            break;
        case ScriptVarType::Int:    inst[d.name] = (int64_t)v.data.i;           break;
        case ScriptVarType::Bool:   inst[d.name] = v.data.b;                    break;
        case ScriptVarType::Float3:
        case ScriptVarType::Color:  inst[d.name] = LuaVec3{ v.data.v3[0], v.data.v3[1], v.data.v3[2] }; break;
        case ScriptVarType::String:
        case ScriptVarType::Asset:  inst[d.name] = v.str;                       break;
        case ScriptVarType::Entity: inst[d.name] = (uint32_t)v.data.entity;     break;
        }
    }
}

// ---------------------------------------------------------------------------
// ApplyExposedVars — push one slot's current var values onto its live instance
// (editor calls this during Play so slider drags show immediately).
// ---------------------------------------------------------------------------
void ScriptSystem::ApplyExposedVars(Entity e, std::size_t slot,
                                    const std::unordered_map<std::string, ScriptVarValue>& overrides)
{
    if (!m_lua) return;
    auto sIt = m_states.find(e);
    if (sIt == m_states.end() || slot >= sIt->second.slots.size()) return;
    const std::string& path = sIt->second.slots[slot].path;
    if (path.empty()) return;
    InjectExposedVars(e, slot, path, &overrides);
}

// ===========================================================================
// LoadSystem — singleton script with OnInit/OnUpdate/OnShutdown (doc §3.3).
// Re-loading the same path tears down the old instance and replaces it.
// ---------------------------------------------------------------------------
bool ScriptSystem::LoadSystem(const std::string& path)
{
    const std::string name = fs::path(path).stem().string();

    // Tear down existing entry with the same name (hot-reload path).
    for (auto& sys : m_systems)
    {
        if (sys.name == name) { TeardownSystem(sys); }
    }

    auto result = m_lua->safe_script_file(path, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        LOG_ERROR("ScriptSystem: system load error [%s]: %s", path.c_str(), err.what());
        return false;
    }

    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        LOG_ERROR("ScriptSystem: system [%s] did not return a table", path.c_str());
        return false;
    }

    sol::table t = ret.as<sol::table>();
    (*m_lua)["__systems"][name] = t;

    // Find or insert. Insertion preserves order; replacement keeps slot.
    SystemEntry* slot = nullptr;
    for (auto& sys : m_systems)
        if (sys.name == name) { slot = &sys; break; }
    if (!slot)
    {
        m_systems.push_back({});
        slot = &m_systems.back();
    }

    slot->name        = name;
    slot->path        = path;
    slot->loaded      = true;
    slot->initCalled  = false;          // OnInit fires on next Update
    slot->hasUpdate   = t["OnUpdate"].valid();
    slot->hasShutdown = t["OnShutdown"].valid();
    slot->lastError.clear();

    try { m_fileTimestamps[path] = fs::last_write_time(path); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded system [%s] from %s (OnInit=%d OnUpdate=%d OnShutdown=%d)",
             name.c_str(), path.c_str(),
             t["OnInit"].valid(), slot->hasUpdate, slot->hasShutdown);
    return true;
}

bool ScriptSystem::RemoveSystem(const std::string& name)
{
    for (auto it = m_systems.begin(); it != m_systems.end(); ++it)
    {
        if (it->name != name) continue;
        TeardownSystem(*it);
        m_systems.erase(it);
        return true;
    }
    return false;
}

void ScriptSystem::TeardownSystem(SystemEntry& sys)
{
    if (sys.loaded && sys.hasShutdown && sys.initCalled)
    {
        sol::table inst = (*m_lua)["__systems"][sys.name];
        if (inst.valid())
        {
            sol::protected_function fn = inst["OnShutdown"];
            if (fn.valid())
            {
                auto res = fn(inst);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnShutdown error [%s]: %s",
                              sys.name.c_str(), err.what());
                }
            }
        }
    }
    (*m_lua)["__systems"][sys.name] = sol::lua_nil;
    sys.loaded = false;
    sys.initCalled = false;
}

// ===========================================================================
// LoadService — singleton stateless table (doc §3.4). No callbacks fired —
// the table is just stored under __services[name] for GetService lookups.
// ---------------------------------------------------------------------------
bool ScriptSystem::LoadService(const std::string& path)
{
    const std::string name = fs::path(path).stem().string();

    auto result = m_lua->safe_script_file(path, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        LOG_ERROR("ScriptSystem: service load error [%s]: %s", path.c_str(), err.what());
        return false;
    }

    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        LOG_ERROR("ScriptSystem: service [%s] did not return a table", path.c_str());
        return false;
    }

    (*m_lua)["__services"][name] = ret.as<sol::table>();

    auto& svc = m_services[name];
    svc.name = name;
    svc.path = path;
    svc.loaded = true;
    svc.lastError.clear();

    try { m_fileTimestamps[path] = fs::last_write_time(path); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded service [%s] from %s", name.c_str(), path.c_str());
    return true;
}

// ===========================================================================
// LoadUIScript — singleton table loaded from asset/scripts/ui/*.lua. Same
// shape as Service (no engine-driven callbacks); convention is that the
// returned table exposes :Open(...) / :Close() that the caller drives via
// Engine.GetUIScript(name).
// ---------------------------------------------------------------------------
bool ScriptSystem::LoadUIScript(const std::string& path)
{
    const std::string name = fs::path(path).stem().string();

    auto result = m_lua->safe_script_file(path, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        LOG_ERROR("ScriptSystem: ui-script load error [%s]: %s", path.c_str(), err.what());
        return false;
    }

    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        LOG_ERROR("ScriptSystem: ui-script [%s] did not return a table", path.c_str());
        return false;
    }

    (*m_lua)["__ui_scripts"][name] = ret.as<sol::table>();

    auto& ui = m_uiScripts[name];
    ui.name = name;
    ui.path = path;
    ui.loaded = true;
    ui.lastError.clear();

    try { m_fileTimestamps[path] = fs::last_write_time(path); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded ui-script [%s] from %s", name.c_str(), path.c_str());
    return true;
}

// ===========================================================================
// DispatchAnimEvent — fire OnAnimEvent(self, name) on the entity's Logic
// instance. Engine code (e.g. AnimationSystem when a clip event fires) can
// call this directly; Lua side has the equivalent Engine.PublishAnimEvent.
// Payload-less form — engine-side anim events rarely carry structured data;
// scripts that need a payload should use Engine.PublishAnimEvent from Lua.
// ---------------------------------------------------------------------------
void ScriptSystem::DispatchAnimEvent(Entity e, const std::string& name)
{
    auto sIt = m_states.find(e);
    if (sIt == m_states.end()) return;
    for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
    {
        if (!sIt->second.slots[i].initCalled) continue;
        sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(e), i);
        if (!inst.valid()) continue;
        sol::protected_function fn = inst["OnAnimEvent"];
        if (!fn.valid()) continue;
        auto res = fn(inst, name, sol::lua_nil);
        if (!res.valid()) {
            sol::error err = res;
            LOG_ERROR("Lua OnAnimEvent error [%s] entity %u slot %zu name=%s: %s",
                      sIt->second.slots[i].path.c_str(), e, i, name.c_str(), err.what());
        }
    }
}

// ===========================================================================
// ScanScriptDirectory — walk root/{services,systems,ui}/*.lua.
// Logic templates are skipped here (they load on first ScriptComponent sight).
// ---------------------------------------------------------------------------
void ScriptSystem::ScanScriptDirectory(const std::string& root)
{
    enum class Kind { Service, System, UI };
    auto scan = [this](const fs::path& dir, Kind kind)
    {
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;

        // Sort alphabetically so load order is deterministic across runs.
        std::vector<fs::path> files;
        for (auto& entry : fs::directory_iterator(dir, ec))
        {
            if (entry.path().extension() == ".lua")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());

        for (const auto& f : files)
        {
            switch (kind)
            {
                case Kind::Service: LoadService (f.string()); break;
                case Kind::System:  LoadSystem  (f.string()); break;
                case Kind::UI:      LoadUIScript(f.string()); break;
            }
        }
    };

    scan(fs::path(root) / "services", Kind::Service);
    scan(fs::path(root) / "systems",  Kind::System);
    scan(fs::path(root) / "ui",       Kind::UI);
}

// ===========================================================================
// Reactive per-entity cleanup hookup.
// ---------------------------------------------------------------------------
void ScriptSystem::BindWorld(World& world)
{
    if (m_boundWorld == &world) return;
    UnbindWorld();
    m_boundWorld = &world;
    m_entityDestroyListenerId = world.AddEntityDestroyListener(
        [this, &world](Entity e) { OnEntityDestroyed(world, e); });
}

void ScriptSystem::UnbindWorld()
{
    if (m_boundWorld && m_entityDestroyListenerId != 0)
        m_boundWorld->RemoveEntityDestroyListener(m_entityDestroyListenerId);
    m_boundWorld              = nullptr;
    m_entityDestroyListenerId = 0;
}

void ScriptSystem::OnEntityDestroyed(World& /*world*/, Entity e)
{
    TeardownEntity(e);   // erase-first + re-entrancy-safe (see TeardownEntity)
}

void ScriptSystem::SweepDestroyed(World& world)
{
    // Collect dead entities first, then tear down: FireSlotDestroy runs OnDestroy
    // (Lua), which can mutate m_states and invalidate iterators. TeardownEntity
    // re-finds each entry, so an entry already cleaned by a prior callback is a
    // harmless no-op.
    std::vector<Entity> dead;
    for (auto& [e, st] : m_states)
        if (!world.IsAlive(e) || !world.HasComponent<ScriptComponent>(e))
            dead.push_back(e);
    for (Entity e : dead)
        TeardownEntity(e);
}

// ---------------------------------------------------------------------------
void ScriptSystem::DispatchLuaEvents()
{
    m_luaBus->Dispatch(*m_lua);
}

// ---------------------------------------------------------------------------
// Bridge a handful of C++ events to the Lua bus as string-keyed table
// payloads. Handlers fire from the C++ EventBus dispatch (main thread, post-
// simulation) and just queue a Lua event; Lua subscribers see them on the
// next ScriptSystem::Update — one-frame latency, acceptable for gameplay.
//
// Entity handles collapse to uint32_t in the Lua payload because Lua isn't
// generation-aware; scripts should still gate on Engine.IsAlive before acting.
// ---------------------------------------------------------------------------
void ScriptSystem::RegisterCppEventBridges()
{
    auto& bus = EventBus::Get();

    {
        const auto sub = bus.Subscribe<FollowTargetLostEvent>(
            [this](const FollowTargetLostEvent& e)
            {
                sol::table t = m_lua->create_table();
                t["follower"]      = static_cast<uint32_t>(e.follower);
                t["formerTarget"]  = static_cast<uint32_t>(e.formerTarget.entity);
                m_luaBus->Publish("FollowTargetLost", std::move(t));
            });
        m_cppBridgeUnsubscribers.push_back(
            [sub] { EventBus::Get().Unsubscribe<FollowTargetLostEvent>(sub); });
    }

    {
        const auto sub = bus.Subscribe<WeaponEquippedEvent>(
            [this](const WeaponEquippedEvent& e)
            {
                sol::table t = m_lua->create_table();
                t["character"]   = static_cast<uint32_t>(e.character.entity);
                t["item"]        = static_cast<uint32_t>(e.item.entity);
                t["socketIndex"] = e.socketIndex;
                m_luaBus->Publish("WeaponEquipped", std::move(t));
            });
        m_cppBridgeUnsubscribers.push_back(
            [sub] { EventBus::Get().Unsubscribe<WeaponEquippedEvent>(sub); });
    }

    {
        const auto sub = bus.Subscribe<WeaponUnequippedEvent>(
            [this](const WeaponUnequippedEvent& e)
            {
                sol::table t = m_lua->create_table();
                t["character"] = static_cast<uint32_t>(e.character.entity);
                t["item"]      = static_cast<uint32_t>(e.item.entity);
                m_luaBus->Publish("WeaponUnequipped", std::move(t));
            });
        m_cppBridgeUnsubscribers.push_back(
            [sub] { EventBus::Get().Unsubscribe<WeaponUnequippedEvent>(sub); });
    }

    {
        const auto sub = bus.Subscribe<ContactBeganEvent>(
            [this](const ContactBeganEvent& e)
            {
                sol::table point  = m_lua->create_table();
                point["x"] = e.point.x; point["y"] = e.point.y; point["z"] = e.point.z;
                sol::table normal = m_lua->create_table();
                normal["x"] = e.normal.x; normal["y"] = e.normal.y; normal["z"] = e.normal.z;

                // 1) Generic broadcast on the Lua bus (anyone can subscribe).
                sol::table t = m_lua->create_table();
                t["bodyA"]  = static_cast<uint32_t>(e.bodyA.entity);
                t["bodyB"]  = static_cast<uint32_t>(e.bodyB.entity);
                t["point"]  = point;
                t["normal"] = normal;
                m_luaBus->Publish("ContactBegan", std::move(t));

                // 2) Targeted Trigger dispatch — fire OnEnter(self, other,
                //    point, normal) on each body's own Logic instance if its
                //    template defined OnEnter. This is what makes any Logic
                //    script that defines OnEnter behave as a Trigger volume
                //    without needing a separate component category.
                auto fire = [&](Entity self, Entity other)
                {
                    auto sIt = m_states.find(self);
                    if (sIt == m_states.end()) return;
                    for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
                    {
                        if (!sIt->second.slots[i].initCalled) continue;
                        sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(self), i);
                        if (!inst.valid()) continue;
                        sol::protected_function fn = inst["OnEnter"];
                        if (!fn.valid()) continue;
                        auto res = fn(inst, static_cast<uint32_t>(other), point, normal);
                        if (!res.valid()) {
                            sol::error err = res;
                            LOG_ERROR("Lua OnEnter error [%s] entity %u slot %zu: %s",
                                      sIt->second.slots[i].path.c_str(), self, i, err.what());
                        }
                    }
                };
                fire(e.bodyA.entity, e.bodyB.entity);
                fire(e.bodyB.entity, e.bodyA.entity);
            });
        m_cppBridgeUnsubscribers.push_back(
            [sub] { EventBus::Get().Unsubscribe<ContactBeganEvent>(sub); });
    }
}

// ===========================================================================
void ScriptSystem::Update(World& world, float dt)
{
    m_elapsed += dt;
    m_world = &world;

    (*m_lua)["Time"]["dt"]      = dt;
    (*m_lua)["Time"]["elapsed"] = m_elapsed;

    SweepDestroyed(world);
    DispatchLuaEvents();

    // ---- Systems: OnInit once, OnUpdate every frame -----------------------
    for (auto& sys : m_systems)
    {
        if (!sys.loaded) continue;
        sol::table inst = (*m_lua)["__systems"][sys.name];
        if (!inst.valid()) continue;

        if (!sys.initCalled)
        {
            sol::protected_function init = inst["OnInit"];
            if (init.valid())
            {
                auto res = init(inst);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnInit error [%s]: %s", sys.name.c_str(), err.what());
                }
            }
            sys.initCalled = true;
        }

        if (sys.hasUpdate)
        {
            sol::protected_function upd = inst["OnUpdate"];
            auto res = upd(inst, dt);
            if (!res.valid()) {
                sol::error err = res;
                LOG_ERROR("Lua OnUpdate error [%s]: %s", sys.name.c_str(), err.what());
                sys.hasUpdate = false; // stop calling broken update
            }
        }
    }

    // ---- Logic: per-entity instances ---------------------------------------
    // Iterate the ScriptComponent pool directly (not world.GetEntities() —
    // that walks ALL entities and HasComponent-filters, which is the documented
    // ECS anti-pattern). Snapshot the entity list because OnSpawn callbacks may
    // call Engine.AttachScript on other entities, which would push into the
    // pool's dense vector mid-iteration; new attachments are picked up next
    // frame.
    auto* scPool = world.GetPool<ScriptComponent>();
    if (!scPool || scPool->Entities().empty()) { m_world = nullptr; return; }

    const std::vector<Entity> entitiesSnapshot(
        scPool->Entities().begin(), scPool->Entities().end());

    // Every Lua callback below (OnSpawn / OnUpdate / teardown OnDestroy) can,
    // via Engine.DestroyEntity, synchronously erase this entity's m_states entry
    // (World fires destroy-listeners inline) or reallocate sc->scripts (a script
    // that AttachScripts to itself). So we hold NO long-lived references across a
    // callback: the component pointer and the m_states iterator are re-fetched
    // after every step, and scalar fields are copied out before use.
    for (Entity e : entitiesSnapshot)
    {
        if (!world.IsAlive(e)) continue;
        auto* sc = scPool->Get(e);
        if (!sc) continue;

        // Ensure per-entity state + capture baseline transform once (shared by
        // all scripts). No Lua runs here, so this short-lived ref is safe.
        {
            auto& st = m_states[e];
            if (!st.baseCaptured)
            {
                if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e))
                    st.baseTransform = *lt;
                st.baseCaptured = true;
            }
        }

        const std::size_t n = sc->scripts.size();

        // Reconcile slot count. Tearing down trailing slots fires OnDestroy,
        // which may destroy this entity — re-find after each and bail if gone.
        {
            auto it = m_states.find(e);
            if (it == m_states.end()) continue;
            if (it->second.slots.size() > n)
            {
                const std::size_t old = it->second.slots.size();
                for (std::size_t i = n; i < old; ++i)
                {
                    DestroyLogicInstance(e, i);
                    if (m_states.find(e) == m_states.end()) break;  // self-destroyed
                }
                it = m_states.find(e);
                if (it == m_states.end()) continue;
                it->second.slots.resize(n);
            }
            else if (it->second.slots.size() < n)
            {
                it->second.slots.resize(n);
            }
        }

        // Drive each slot independently.
        for (std::size_t i = 0; i < n; ++i)
        {
            sc = scPool->Get(e);
            auto it = m_states.find(e);
            if (!sc || it == m_states.end()) break;             // entity gone
            if (i >= sc->scripts.size() || i >= it->second.slots.size()) break;

            const std::string path    = sc->scripts[i].scriptPath;  // copy
            const bool        enabled = sc->scripts[i].enabled;

            // Path changed at this slot (first sight, edited path, or a Remove
            // shifted entries up) → tear down the old instance and reset meta.
            if (it->second.slots[i].path != path)
            {
                if (!it->second.slots[i].path.empty())
                {
                    DestroyLogicInstance(e, i);            // fires OnDestroy (Lua)
                    it = m_states.find(e);
                    if (it == m_states.end()) break;
                    if (i >= it->second.slots.size()) break;
                }
                it->second.slots[i] = {};
                it->second.slots[i].path = path;
            }

            if (path.empty() || !enabled) continue;

            // Ensure template loaded.
            if (m_logicTemplates.find(path) == m_logicTemplates.end())
            {
                if (!LoadLogicTemplate(path)) continue;
            }

            // Ensure instance + fire OnSpawn once for this slot. The vars
            // pointer is consumed (InjectExposedVars) before OnSpawn runs, so it
            // is safe even though OnSpawn may later reallocate the vector. A
            // failed spawn leaves initCalled=false (retry next frame) and skips
            // OnUpdate this frame.
            if (!it->second.slots[i].initCalled)
            {
                const bool ok = EnsureLogicInstance(e, i, path, &sc->scripts[i].vars);
                it = m_states.find(e);
                if (it == m_states.end()) break;          // OnSpawn destroyed e
                if (i >= it->second.slots.size()) break;
                if (!ok) continue;
                it->second.slots[i].initCalled = true;
            }

            // Per-frame OnUpdate(self, dt).
            auto tmplIt = m_logicTemplates.find(path);
            if (tmplIt != m_logicTemplates.end() && tmplIt->second.hasUpdate)
            {
                sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(e), i);
                if (!inst.valid()) continue;

                sol::protected_function fn = inst["OnUpdate"];
                auto res = fn(inst, dt);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnUpdate error [%s] entity %u slot %zu: %s",
                              path.c_str(), e, i, err.what());
                    // Disable only this slot to stop spamming — other scripts on
                    // the same entity keep running. Re-fetch in case OnUpdate
                    // moved the component / destroyed the entity, and bounds-check.
                    sc = scPool->Get(e);
                    if (sc && i < sc->scripts.size()) sc->scripts[i].enabled = false;
                }
            }
        }
    }

    m_world = nullptr;
}

// ===========================================================================
// CheckHotReload — for each tracked file, if mtime changed:
//   - Logic template: re-load template; live instances re-bind to the new
//     template via metatable __index (their per-entity state is preserved).
//   - System: tear down (OnShutdown) + LoadSystem (OnInit fires next Update).
//   - Service: re-load — Lua callers caching the table see the old one until
//     they re-fetch via Engine.GetService.
// ---------------------------------------------------------------------------
void ScriptSystem::CheckHotReload(World& /*world*/)
{
    for (auto& [path, oldTime] : m_fileTimestamps)
    {
        try {
            auto newTime = fs::last_write_time(path);
            if (newTime == oldTime) continue;
            oldTime = newTime;

            // Logic template?
            if (m_logicTemplates.find(path) != m_logicTemplates.end())
            {
                LOG_INFO("ScriptSystem: hot-reload logic template [%s]", path.c_str());
                if (!LoadLogicTemplate(path)) continue;
                // Re-point every live instance's metatable.__index at the
                // freshly loaded template. Per-slot self.entity / self.slot /
                // cached fields survive untouched. An entity may have several
                // slots bound to the same template — re-point each.
                sol::table newTmpl = (*m_lua)["__logic_templates"][path];
                for (auto& [entity, st] : m_states)
                {
                    for (std::size_t i = 0; i < st.slots.size(); ++i)
                    {
                        if (st.slots[i].path != path) continue;
                        sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(entity), i);
                        if (!inst.valid()) continue;
                        sol::table mt = m_lua->create_table();
                        mt["__index"] = newTmpl;
                        inst[sol::metatable_key] = mt;
                    }
                }
                continue;
            }

            // System?
            bool isSystem = false;
            for (const auto& sys : m_systems)
                if (sys.path == path) { isSystem = true; break; }
            if (isSystem)
            {
                LOG_INFO("ScriptSystem: hot-reload system [%s]", path.c_str());
                LoadSystem(path);
                continue;
            }

            // Service?
            bool wasService = false;
            for (const auto& [name, svc] : m_services)
            {
                if (svc.path != path) continue;
                wasService = true;
                LOG_INFO("ScriptSystem: hot-reload service [%s]", path.c_str());
                LoadService(path);
                break;
            }
            if (wasService) continue;

            // UI script?
            for (const auto& [name, ui] : m_uiScripts)
            {
                if (ui.path != path) continue;
                LOG_INFO("ScriptSystem: hot-reload ui-script [%s]", path.c_str());
                LoadUIScript(path);
                break;
            }
        } catch (...) {}
    }
}
