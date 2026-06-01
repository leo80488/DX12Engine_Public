#include "ECS/LuaCharacterStateBindings.h"
#include "ECS/CharacterStateComponent.h"
#include "ECS/ECS.h"

#include "System/Log.h"

#include <sol/sol.hpp>

#include <algorithm>

namespace
{

CharacterStateComponent& EnsureComp(World& w, Entity e)
{
    if (auto* p = w.GetComponent<CharacterStateComponent>(e)) return *p;
    w.AddComponent<CharacterStateComponent>(e, CharacterStateComponent{});
    return *w.GetComponent<CharacterStateComponent>(e);
}

int FindByName(const CharacterStateComponent& cs, const std::string& name)
{
    for (size_t i = 0; i < cs.states.size(); ++i)
        if (cs.states[i].name == name) return static_cast<int>(i);
    return -1;
}

} // namespace

void RegisterLuaCharacterStateBindings(sol::state& lua, World& world)
{
    // Merge into any pre-existing Character table — see the matching note in
    // LuaPlayerBindings.cpp. Registration order between the two bindings can
    // swap with refactors; both sides need to be merge-safe.
    sol::object existing = lua["Character"];
    sol::table tbl = existing.is<sol::table>()
                   ? existing.as<sol::table>()
                   : lua.create_table();

    tbl.set_function("AddState",
        [&world](uint32_t entityId, const std::string& name,
                 const std::string& clipPath, sol::object optsObj) -> bool
        {
            const Entity e = static_cast<Entity>(entityId);
            if (!world.IsAlive(e)) return false;

            CharacterStateComponent& cs = EnsureComp(world, e);
            const int idx = FindByName(cs, name);

            CharacterStateComponent::StateEntry st;
            st.name     = name;
            st.clipPath = clipPath;
            // Defaults; an opts table can override individually.
            st.looping       = true;
            st.playbackSpeed = 1.0f;
            if (optsObj.is<sol::table>())
            {
                sol::table o = optsObj.as<sol::table>();
                if (o["loop"].valid())  st.looping       = o.get_or("loop",  true);
                if (o["speed"].valid()) st.playbackSpeed = o.get_or("speed", 1.0f);
            }

            if (idx >= 0)
            {
                // Same-name re-registration overwrites the entry but keeps
                // its slot — preserves currentIdx / pendingIdx pointers that
                // may already reference it.
                auto& dst = cs.states[idx];
                const bool clipChanged = (dst.clipPath != st.clipPath);
                dst.clipPath      = st.clipPath;
                dst.looping       = st.looping;
                dst.playbackSpeed = st.playbackSpeed;
                if (clipChanged)
                {
                    dst.clipLibIdx    = kInvalidClipIndex;  // force re-bind
                    dst.clipResHandle = {};
                }
            }
            else
            {
                cs.states.push_back(std::move(st));
            }
            return true;
        });

    tbl.set_function("SetState",
        [&world](uint32_t entityId, const std::string& name,
                 sol::object blendObj) -> bool
        {
            const Entity e = static_cast<Entity>(entityId);
            if (!world.IsAlive(e)) return false;
            auto* cs = world.GetComponent<CharacterStateComponent>(e);
            if (!cs) { LOG_WARNING("Character.SetState: entity %u has no CharacterStateComponent", entityId); return false; }

            const int idx = FindByName(*cs, name);
            if (idx < 0) { LOG_WARNING("Character.SetState: state '%s' not registered on entity %u", name.c_str(), entityId); return false; }

            // Same as current AND no blend in flight → no-op. Re-triggering a
            // state mid-blend swaps the target which is intentional (e.g.
            // IDLE → WALK → cancel back to IDLE before WALK finishes).
            if (idx == cs->currentIdx && cs->pendingIdx < 0) return true;

            // Already blending toward this exact state — let the in-flight
            // blend continue. Without this guard, callers that issue
            // SetState every BT tick (e.g. `SetAnimState("RUN", 0.15)` in a
            // Chase loop) reset blendElapsed to 0 each frame, so the blend
            // never completes and the cross-fade visually lingers forever.
            if (idx == cs->pendingIdx) return true;

            // Blend duration: explicit argument wins; otherwise use the
            // component's default.
            float dur = cs->defaultBlendDuration;
            if (blendObj.is<float>())  dur = blendObj.as<float>();
            else if (blendObj.is<int>()) dur = static_cast<float>(blendObj.as<int>());
            cs->blendDuration = std::max(0.0f, dur);
            cs->blendElapsed  = 0.0f;
            cs->pendingIdx    = idx;
            return true;
        });

    tbl.set_function("GetState",
        [&world](sol::this_state ts, uint32_t entityId) -> sol::object
        {
            sol::state_view L(ts);
            const Entity e = static_cast<Entity>(entityId);
            if (!world.IsAlive(e)) return sol::lua_nil;
            const auto* cs = world.GetComponent<CharacterStateComponent>(e);
            if (!cs || cs->currentIdx < 0 ||
                static_cast<size_t>(cs->currentIdx) >= cs->states.size())
                return sol::lua_nil;
            return sol::make_object(L, cs->states[cs->currentIdx].name);
        });

    tbl.set_function("IsBlending",
        [&world](uint32_t entityId) -> bool
        {
            const Entity e = static_cast<Entity>(entityId);
            if (!world.IsAlive(e)) return false;
            const auto* cs = world.GetComponent<CharacterStateComponent>(e);
            return cs && cs->pendingIdx >= 0;
        });

    lua["Character"] = tbl;
    LOG_INFO("Character: registered Lua bindings");
}
