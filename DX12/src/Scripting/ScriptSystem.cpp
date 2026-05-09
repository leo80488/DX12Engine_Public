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
#include "System/EventBus.h"
#include "System/Log.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <cmath>
#include <cstring>

using namespace DirectX;

// ===========================================================================
// ---------------------------------------------------------------------------
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

    // Unsubscribe our bridge lambdas BEFORE the Lua state goes away — they
    // capture `this` and m_lua, so a post-destruction dispatch would blow up.
    for (auto& unsub : m_cppBridgeUnsubscribers) unsub();
    m_cppBridgeUnsubscribers.clear();

    // Drop every container that may hold sol::function / sol::object refs
    // BEFORE deleting m_lua — otherwise their destructors run *after*
    // `delete m_lua` (as members destruct in reverse declaration order)
    // and call luaL_unref on a freed lua_State.  Reproducing crash:
    //   sol::basic_reference::~basic_reference → deref → luaL_unref → ASAN/UAF
    // ClearAll() empties state pools without touching m_lua so bindings
    // stay valid until we actually delete it on the next line.
    ClearAll();

    delete m_lua;
}

void ScriptSystem::ClearAll()
{
    m_states.clear();
    m_globalStates.clear();
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

    // Compact when more than half of entries are dead.
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
    // type is LightType enum: 0=Directional, 1=Point, 2=Spot (stored as int in Lua)
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

    // LightType constants (mirrors the C++ enum ordering).
    lua["LightType"] = lua.create_table_with(
        "Directional", 0, "Point", 1, "Spot", 2);

    // ---- CameraComponent ----
    lua.new_usertype<CameraComponent>("Camera",
        "yaw",              &CameraComponent::yaw,
        "pitch",            &CameraComponent::pitch,
        "fov",              &CameraComponent::fov,
        "nearZ",            &CameraComponent::nearZ,
        "farZ",             &CameraComponent::farZ,
        "mouseSensitivity", &CameraComponent::mouseSensitivity,
        "moveSpeed",        &CameraComponent::moveSpeed,
        "position", sol::property(
            [](CameraComponent& c) -> LuaVec3 { return {c.position.x, c.position.y, c.position.z}; },
            [](CameraComponent& c, LuaVec3 v) { c.position = {v.x, v.y, v.z}; })
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
    auto input = lua.create_table();
    input.set_function("IsKeyDown", [](int key) -> bool {
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    });
    input.set_function("IsMouseDown", [](int button) -> bool {
        // button: 0=Left, 1=Right, 2=Middle
        int vk = (button == 1) ? VK_RBUTTON : (button == 2) ? VK_MBUTTON : VK_LBUTTON;
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    });
    // Screen-space cursor position (window-relative when the game owns the
    // foreground window). Returns (x, y) as two floats.
    input.set_function("GetMousePos", []() -> std::tuple<float,float> {
        POINT p; GetCursorPos(&p);
        if (HWND hwnd = GetForegroundWindow())
            ScreenToClient(hwnd, &p);
        return { static_cast<float>(p.x), static_cast<float>(p.y) };
    });
    lua["Input"] = input;

    lua["Mouse"] = lua.create_table_with("Left", 0, "Right", 1, "Middle", 2);

    // ---- Key constants ----
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

    // ---- Engine.* Command API (see include/ECS/CommandAPI.h) ----------------
    // These are the only sanctioned way for Lua to mutate equipment / follow
    // bindings. Lambdas capture `this` and reach the current frame's World
    // via m_world, which Update() sets before running scripts.
    auto engine = lua.create_table();

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

    // Adds a ScriptComponent so the entity gets its own Lua environment in
    // future frames (the path is loaded lazily in Update's per-entity loop).
    engine.set_function("AttachScript",
        [this](uint32_t id, const std::string& path) -> bool
        {
            if (!m_world) return false;
            if (!m_world->IsAlive(static_cast<Entity>(id))) return false;
            ScriptComponent sc;
            sc.scriptPath = path;
            sc.enabled    = true;
            m_world->AddComponent(static_cast<Entity>(id), sc);
            return true;
        });

    // ---- Component access ---------------------------------------------------
    // Return raw pointers to components. sol binds them as the registered
    // usertype, so Lua can read/write fields directly. Returns nil if the
    // entity is dead or lacks the component — Lua-side idiom:
    //     local cam = Engine.GetCamera(id)
    //     if cam then cam.fov = 1.2 end
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

    engine.set_function("GetAnimation",
        [this](uint32_t id) -> AnimationComponent*
        {
            if (!m_world) return nullptr;
            return m_world->GetComponent<AnimationComponent>(static_cast<Entity>(id));
        });

    // Name-keyed existence check for components currently exposed to Lua.
    // Keep the list in sync with the Get* functions above.
    engine.set_function("HasComponent",
        [this](uint32_t id, const std::string& name) -> bool
        {
            if (!m_world) return false;
            const Entity e = static_cast<Entity>(id);
            if      (name == "LocalTransform")    return m_world->HasComponent<LocalTransform>(e);
            else if (name == "Light")             return m_world->HasComponent<LightData>(e);
            else if (name == "Camera")            return m_world->HasComponent<CameraComponent>(e);
            else if (name == "Animation")         return m_world->HasComponent<AnimationComponent>(e);
            else if (name == "Script")            return m_world->HasComponent<ScriptComponent>(e);
            else if (name == "Tag")               return m_world->HasComponent<TagComponent>(e);
            LOG_WARNING("Engine.HasComponent: unknown component name '%s'", name.c_str());
            return false;
        });

    // ---- Tags ---------------------------------------------------------------
    // TagComponent is lazily added on first AddTag call so entities stay
    // slim until they actually need a tag list.
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

    // ---- Game time scale (hit-stop / bullet-time) ---------------------------
    // SetTimeScale clamps to >= 0; 0 == full freeze.
    engine.set_function("SetTimeScale",
        [this](float s) { SetTimeScale(s); });
    engine.set_function("GetTimeScale",
        [this]() -> float { return GetTimeScale(); });

    // ---- AfterDelay — schedule a Lua callback after N real seconds ----------
    // Real-time delay (NOT affected by time scale) so hit-stop expirations
    // actually fire while the world is frozen at scale ~= 0. Returns a handle
    // usable with CancelDelay; 0 on bad input.
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
    // Queued: Publish enqueues, subscribers run on next Dispatch (top of
    // ScriptSystem::Update). Events published inside a handler land in the
    // following dispatch — same no-reentrancy contract as the C++ bus.
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
                t = m_lua->create_table();   // allow Publish("Name") with no payload
            m_luaBus->Publish(name, std::move(t));
        });

    lua["Engine"] = engine;

    // ---- Per-entity environment storage (used by LoadScript) ----
    lua["__entity_envs"] = lua.create_table();
    // ---- Per-path storage for global scripts (used by LoadGlobalScript) ----
    lua["__global_envs"] = lua.create_table();
}

// ===========================================================================
void ScriptSystem::LoadScript(Entity e, const std::string& path, World& world)
{
    auto& st = m_states[e];
    st.path = path;
    st.loaded = false;
    st.hasInit = false;
    st.hasUpdate = false;
    st.initCalled = false;
    st.lastError.clear();

    // Create per-entity environment (sandbox).
    sol::environment env(*m_lua, sol::create, m_lua->globals());

    // Inject entity helpers.
    env.set_function("GetLocalTransform", [&world, e]() -> LocalTransform* {
        return world.GetComponent<LocalTransform>(e);
    });
    env.set_function("GetName", [&world, e]() -> std::string {
        return world.GetName(e);
    });
    env.set_function("GetEntityID", [e]() -> uint32_t { return e; });

    // ---- Baseline helpers — see ScriptState::baseTransform for rationale. --
    // Positions / rotations / scales are returned as fresh tables so scripts
    // can mutate the local copy without affecting the stored baseline.
    env.set_function("GetBasePosition", [this, e]() -> sol::table {
        sol::table t = m_lua->create_table();
        const auto it = m_states.find(e);
        if (it != m_states.end() && it->second.baseCaptured) {
            const auto& p = it->second.baseTransform.translation;
            t["x"] = p.x; t["y"] = p.y; t["z"] = p.z;
        } else {
            t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f;
        }
        return t;
    });
    env.set_function("GetBaseRotation", [this, e]() -> sol::table {
        sol::table t = m_lua->create_table();
        const auto it = m_states.find(e);
        if (it != m_states.end() && it->second.baseCaptured) {
            const auto& r = it->second.baseTransform.rotation;
            t["x"] = r.x; t["y"] = r.y; t["z"] = r.z; t["w"] = r.w;
        } else {
            t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f; t["w"] = 1.f;
        }
        return t;
    });
    env.set_function("GetBaseScale", [this, e]() -> sol::table {
        sol::table t = m_lua->create_table();
        const auto it = m_states.find(e);
        if (it != m_states.end() && it->second.baseCaptured) {
            const auto& s = it->second.baseTransform.scale;
            t["x"] = s.x; t["y"] = s.y; t["z"] = s.z;
        } else {
            t["x"] = 1.f; t["y"] = 1.f; t["z"] = 1.f;
        }
        return t;
    });
    // Explicit rebase — typical use: script wants "current pose is now the
    // rest pose" after warping the entity (teleport, cutscene end).
    env.set_function("ResetBase", [this, e, &world]() {
        auto it = m_states.find(e);
        if (it == m_states.end()) return;
        if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e)) {
            it->second.baseTransform = *lt;
            it->second.baseCaptured  = true;
        }
    });

    auto result = m_lua->script_file(path, env);
    if (!result.valid())
    {
        sol::error err = result;
        st.lastError = err.what();
        LOG_ERROR("ScriptSystem: load error [%s]: %s", path.c_str(), err.what());
        return;
    }

    // Store the environment in the Lua registry keyed by entity ID.
    (*m_lua)["__entity_envs"][e] = env;

    st.loaded     = true;
    st.hasInit    = env["Init"].valid();
    st.hasUpdate  = env["Update"].valid();
    st.hasDestroy = env["OnDestroy"].valid();

    // Track file timestamp for hot reload.
    try {
        m_fileTimestamps[path] = std::filesystem::last_write_time(path);
    } catch (...) {}

    LOG_INFO("ScriptSystem: loaded [%s] for entity %u (Init=%d Update=%d OnDestroy=%d)",
             path.c_str(), e, st.hasInit, st.hasUpdate, st.hasDestroy);
}

// ===========================================================================
// Global scripts — game-logic owners (InputHandler, AbilitySystem, …). Stored
// in __global_envs[path] so Update can pick each one back up; otherwise the
// lifecycle mirrors entity scripts.
void ScriptSystem::AddGlobalScript(const std::string& path)
{
    if (m_globalStates.count(path)) return;            // idempotent
    m_globalStates[path] = {};                          // reserve slot; LoadGlobalScript fills
    LoadGlobalScript(path);
}

void ScriptSystem::RemoveGlobalScript(const std::string& path)
{
    auto it = m_globalStates.find(path);
    if (it == m_globalStates.end()) return;

    // Fire OnDestroy (if present) before we drop the env.
    if (it->second.loaded && it->second.hasDestroy)
    {
        sol::environment env = (*m_lua)["__global_envs"][path];
        if (env.valid())
        {
            sol::protected_function fn = env["OnDestroy"];
            if (fn.valid())
            {
                auto res = fn();
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnDestroy error [%s]: %s", path.c_str(), err.what());
                }
            }
        }
    }

    (*m_lua)["__global_envs"][path] = sol::lua_nil;
    m_globalStates.erase(it);
}

void ScriptSystem::LoadGlobalScript(const std::string& path)
{
    auto& st = m_globalStates[path];
    st.path = path;
    st.loaded = false;
    st.hasInit = false;
    st.hasUpdate = false;
    st.hasDestroy = false;
    st.initCalled = false;
    st.lastError.clear();

    sol::environment env(*m_lua, sol::create, m_lua->globals());
    // No entity helpers — global scripts operate on the world via Engine.*

    auto result = m_lua->script_file(path, env);
    if (!result.valid())
    {
        sol::error err = result;
        st.lastError = err.what();
        LOG_ERROR("ScriptSystem: global load error [%s]: %s", path.c_str(), err.what());
        return;
    }

    (*m_lua)["__global_envs"][path] = env;

    st.loaded     = true;
    st.hasInit    = env["Init"].valid();
    st.hasUpdate  = env["Update"].valid();
    st.hasDestroy = env["OnDestroy"].valid();

    try { m_fileTimestamps[path] = std::filesystem::last_write_time(path); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded global [%s] (Init=%d Update=%d OnDestroy=%d)",
             path.c_str(), st.hasInit, st.hasUpdate, st.hasDestroy);
}

// ---------------------------------------------------------------------------
// SweepDestroyed — run OnDestroy for entities whose ScriptComponent went away
// (either the entity was destroyed, or the component was removed). Called at
// the top of Update so the rest of the frame works with a clean state set.
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
    auto it = m_states.find(e);
    if (it == m_states.end()) return;

    // Fire OnDestroy before we drop the Lua env so script authors observe
    // a still-valid entity during teardown. Same logic as SweepDestroyed,
    // lifted here so it runs SYNCHRONOUSLY with World::DestroyEntity instead
    // of on the next Update tick.
    if (it->second.loaded && it->second.hasDestroy && it->second.initCalled)
    {
        sol::environment env = (*m_lua)["__entity_envs"][e];
        if (env.valid())
        {
            sol::protected_function fn = env["OnDestroy"];
            if (fn.valid())
            {
                auto res = fn();
                if (!res.valid())
                {
                    sol::error err = res;
                    LOG_ERROR("Lua OnDestroy error [%s] entity %u: %s",
                              it->second.path.c_str(), e, err.what());
                }
            }
        }
    }
    (*m_lua)["__entity_envs"][e] = sol::lua_nil;
    m_states.erase(it);
}

void ScriptSystem::SweepDestroyed(World& world)
{
    for (auto it = m_states.begin(); it != m_states.end(); )
    {
        const Entity e = it->first;
        const bool gone = !world.IsAlive(e) || !world.HasComponent<ScriptComponent>(e);
        if (!gone) { ++it; continue; }

        if (it->second.loaded && it->second.hasDestroy && it->second.initCalled)
        {
            sol::environment env = (*m_lua)["__entity_envs"][e];
            if (env.valid())
            {
                sol::protected_function fn = env["OnDestroy"];
                if (fn.valid())
                {
                    auto res = fn();
                    if (!res.valid()) {
                        sol::error err = res;
                        LOG_ERROR("Lua OnDestroy error [%s] entity %u: %s",
                                  it->second.path.c_str(), e, err.what());
                    }
                }
            }
        }

        (*m_lua)["__entity_envs"][e] = sol::lua_nil;
        it = m_states.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Drain the Lua event queue once. Called at the top of Update so handlers
// see a consistent pre-frame state before any scripts run this frame.
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
void ScriptSystem::RegisterCppEventBridges()
{
    auto& bus = EventBus::Get();

    // FollowTargetLost
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

    // WeaponEquipped
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

    // WeaponUnequipped
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

    // ContactBegan (physics)
    {
        const auto sub = bus.Subscribe<ContactBeganEvent>(
            [this](const ContactBeganEvent& e)
            {
                sol::table t = m_lua->create_table();
                t["bodyA"]  = static_cast<uint32_t>(e.bodyA.entity);
                t["bodyB"]  = static_cast<uint32_t>(e.bodyB.entity);
                sol::table point  = m_lua->create_table();
                point["x"] = e.point.x; point["y"] = e.point.y; point["z"] = e.point.z;
                sol::table normal = m_lua->create_table();
                normal["x"] = e.normal.x; normal["y"] = e.normal.y; normal["z"] = e.normal.z;
                t["point"]  = point;
                t["normal"] = normal;
                m_luaBus->Publish("ContactBegan", std::move(t));
            });
        m_cppBridgeUnsubscribers.push_back(
            [sub] { EventBus::Get().Unsubscribe<ContactBeganEvent>(sub); });
    }
}

// ===========================================================================
void ScriptSystem::Update(World& world, float dt)
{
    m_elapsed += dt;

    // Publish the current frame's world to Engine.* Lua bindings. Stored for
    // the duration of this call; cleared at the bottom so any Lua invocation
    // happening outside Update (which shouldn't happen in normal flow) sees
    // a null world rather than a dangling pointer.
    m_world = &world;

    // Update Time table.
    (*m_lua)["Time"]["dt"]      = dt;
    (*m_lua)["Time"]["elapsed"] = m_elapsed;

    // Fire OnDestroy for any entity that lost its ScriptComponent between the
    // previous frame and this one, then drop its env. Done first so later
    // stages work with a pruned state map.
    SweepDestroyed(world);

    // Deliver events queued by C++ systems last frame (and any Lua publishes
    // from the previous frame that missed the window). Subscribers may
    // Publish new events; those land in next frame's dispatch.
    DispatchLuaEvents();

    // ---- Global scripts: Init once, Update every frame ---------------------
    for (auto& [path, st] : m_globalStates)
    {
        if (!st.loaded) continue;
        sol::environment env = (*m_lua)["__global_envs"][path];
        if (!env.valid()) continue;

        if (st.hasInit && !st.initCalled)
        {
            sol::protected_function fn = env["Init"];
            auto res = fn();
            if (!res.valid()) {
                sol::error err = res;
                LOG_ERROR("Lua Init error [%s]: %s", path.c_str(), err.what());
            }
            st.initCalled = true;
        }
        if (st.hasUpdate)
        {
            sol::protected_function fn = env["Update"];
            auto res = fn(dt);
            if (!res.valid()) {
                sol::error err = res;
                LOG_ERROR("Lua Update error [%s]: %s", path.c_str(), err.what());
                st.hasUpdate = false;
            }
        }
    }

    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;
        auto* sc = world.GetComponent<ScriptComponent>(e);
        if (!sc || sc->scriptPath.empty() || !sc->enabled) continue;

        auto& st = m_states[e];

        // Lazy load / reload on path change.
        if (!st.loaded || st.path != sc->scriptPath)
            LoadScript(e, sc->scriptPath, world);
        if (!st.loaded) continue;

        sol::environment env = (*m_lua)["__entity_envs"][e];

        // Snapshot the entity's LocalTransform the first time the script
        // sees it so Init (and Update) can read GetBasePosition etc. If the
        // entity has no LocalTransform yet, fall through with default-
        // constructed zeros — the user can call ResetBase later once a
        // transform is attached. Preserved across hot reloads.
        if (!st.baseCaptured)
        {
            if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e))
                st.baseTransform = *lt;
            st.baseCaptured = true;
        }

        // One-shot Init().
        if (st.hasInit && !st.initCalled)
        {
            sol::protected_function fn = env["Init"];
            auto res = fn();
            if (!res.valid()) {
                sol::error err = res;
                LOG_ERROR("Lua Init error [%s]: %s", st.path.c_str(), err.what());
            }
            st.initCalled = true;
        }

        // Per-frame Update(dt).
        if (st.hasUpdate)
        {
            sol::protected_function fn = env["Update"];
            auto res = fn(dt);
            if (!res.valid()) {
                sol::error err = res;
                LOG_ERROR("Lua Update error [%s]: %s", st.path.c_str(), err.what());
                st.hasUpdate = false; // stop calling broken script
            }
        }
    }

    m_world = nullptr;
}

// ===========================================================================
void ScriptSystem::CheckHotReload(World& world)
{
    for (auto& [path, oldTime] : m_fileTimestamps)
    {
        try {
            auto newTime = std::filesystem::last_write_time(path);
            if (newTime != oldTime)
            {
                oldTime = newTime;
                // Reload all entities using this script.
                for (auto& [entity, st] : m_states)
                {
                    if (st.path == path)
                    {
                        st.loaded = false; // triggers reload next Update
                        LOG_INFO("ScriptSystem: hot-reload detected for [%s]", path.c_str());
                    }
                }
            }
        } catch (...) {}
    }
}
