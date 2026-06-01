#include "ECS/LuaPlayerBindings.h"
#include "ECS/PlayerComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/Components.h"           // CameraComponent + ActiveCameraTag
#include "ECS/HierarchyComponents.h"
#include "ECS/ECS.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/CameraStackSystem.h"
#include "System/Log.h"

#include <sol/sol.hpp>
#include <cstring>

namespace
{
    CharacterControllerComponent* GetCC(World& w, Entity e)
    {
        if (auto* cc = w.GetComponent<CharacterControllerComponent>(e)) return cc;
        LOG_WARNING("Lua: entity %u has no CharacterControllerComponent", e);
        return nullptr;
    }
    PlayerComponent* GetPC(World& w, Entity e)
    {
        if (auto* p = w.GetComponent<PlayerComponent>(e)) return p;
        LOG_WARNING("Lua: entity %u has no PlayerComponent", e);
        return nullptr;
    }
}

void RegisterLuaPlayerBindings(sol::state& lua, World& world)
{
    // ---- Character.* ---------------------------------------------------
    // Merge into any pre-existing Character table (e.g. populated by
    // RegisterLuaCharacterStateBindings) instead of replacing it — the FSM
    // bindings own AddState/SetState/GetState/IsBlending under the same
    // namespace, and a fresh create_table() would silently clobber them.
    sol::object existingCh = lua["Character"];
    sol::table ch = existingCh.is<sol::table>()
                  ? existingCh.as<sol::table>()
                  : lua.create_table();

    ch.set_function("IsGrounded",
        [&world](uint32_t entityId) -> bool {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            return cc ? cc->isGrounded : false;
        });

    ch.set_function("TimeInAir",
        [&world](uint32_t entityId) -> float {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            return cc ? cc->timeInAir : 0.f;
        });

    // Returns world-space velocity (vx, vy, vz). Lua-side: `local x,y,z = Character.GetVelocity(e)`.
    ch.set_function("GetVelocity",
        [&world](uint32_t entityId) -> std::tuple<float,float,float> {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            if (!cc) return {0.f, 0.f, 0.f};
            return { cc->velocity.x, cc->velocity.y, cc->velocity.z };
        });

    // Ground info: normal + entity ID standing on. Returns (nx, ny, nz, entityId).
    ch.set_function("GetGround",
        [&world](uint32_t entityId) -> std::tuple<float,float,float,uint32_t> {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            if (!cc) return {0.f, 1.f, 0.f, NullEntity};
            return { cc->groundNormal.x, cc->groundNormal.y, cc->groundNormal.z,
                     static_cast<uint32_t>(cc->groundEntity) };
        });

    // Override the drive horizontal velocity for this frame. PlayerControllerSystem
    // will normally overwrite this — calls from Lua are only useful when:
    //   • the entity has no PlayerComponent (NPCs / cinematic puppets), OR
    //   • a Lua-driven cutscene wants to force motion. In the second case,
    //     also disable the player controller for that entity (e.g. by
    //     stripping PlayerComponent or via a custom flag).
    ch.set_function("SetDesiredVelocity",
        [&world](uint32_t entityId, float vx, float vz) -> bool {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            if (!cc) return false;
            cc->desiredHorizontalVelocity = { vx, 0.f, vz };
            return true;
        });

    // Request a jump on the next physics tick. PhysicsSystem honours it only
    // when the character is grounded (or within coyote window if called via
    // PlayerControllerSystem — but direct calls bypass coyote/buffer logic).
    ch.set_function("Jump",
        [&world](uint32_t entityId) -> bool {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            if (!cc) return false;
            cc->jumpRequested = true;
            return true;
        });

    // Hard teleport: snap LocalTransform.translation. The KCC's
    // CharacterVirtual reads its position from the entity pose on the next
    // tick via PhysicsSystem's lazy-build path — but that path only fires on
    // the first frame the CCC is seen. For an established CCC, we need to
    // also bump generation so PhysicsSystem rebuilds the CV at the new pose.
    ch.set_function("Teleport",
        [&world](uint32_t entityId, float x, float y, float z) -> bool {
            const Entity e = static_cast<Entity>(entityId);
            auto* cc = GetCC(world, e);
            auto* lt = world.GetComponent<LocalTransform>(e);
            if (!cc || !lt) return false;
            lt->translation = { x, y, z };
            cc->MarkDirty();   // forces CharacterVirtual rebuild at new pose
            return true;
        });

    // Capsule resize. Bumps generation so PhysicsSystem rebuilds.
    ch.set_function("SetCapsule",
        [&world](uint32_t entityId, float radius, float halfHeight) -> bool {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            if (!cc) return false;
            cc->capsuleRadius     = radius;
            cc->capsuleHalfHeight = halfHeight;
            cc->MarkDirty();
            return true;
        });

    ch.set_function("SetGravity",
        [&world](uint32_t entityId, float g) -> bool {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            if (!cc) return false;
            cc->gravity = g;
            return true;
        });

    ch.set_function("SetJumpSpeed",
        [&world](uint32_t entityId, float s) -> bool {
            auto* cc = GetCC(world, static_cast<Entity>(entityId));
            if (!cc) return false;
            cc->jumpSpeed = s;
            return true;
        });

    lua["Character"] = ch;

    // ---- Player.* ------------------------------------------------------
    sol::table pl = lua.create_table();

    pl.set_function("SetWalkSpeed",
        [&world](uint32_t entityId, float s) -> bool {
            auto* pc = GetPC(world, static_cast<Entity>(entityId));
            if (!pc) return false;
            pc->walkSpeed = s;
            return true;
        });

    pl.set_function("SetRunSpeed",
        [&world](uint32_t entityId, float s) -> bool {
            auto* pc = GetPC(world, static_cast<Entity>(entityId));
            if (!pc) return false;
            pc->runSpeed = s;
            return true;
        });

    pl.set_function("SetAirControl",
        [&world](uint32_t entityId, float c) -> bool {
            auto* pc = GetPC(world, static_cast<Entity>(entityId));
            if (!pc) return false;
            pc->airControl = c;
            return true;
        });

    pl.set_function("SetCameraEntity",
        [&world](uint32_t entityId, uint32_t cameraId) -> bool {
            auto* pc = GetPC(world, static_cast<Entity>(entityId));
            if (!pc) return false;
            // BindAndStamp auto-adds a GuidComponent to the camera entity if
            // it doesn't already have one, so this Lua call survives the
            // scene save/load round-trip.
            pc->cameraEntity.BindAndStamp(world, static_cast<Entity>(cameraId));
            return true;
        });

    // Convenience: in one call, drop in CharacterController + Player + zero
    // out LocalTransform. Saves Lua callers from doing the three-add dance
    // for the common "create a player avatar at origin" pattern. Existing
    // components on the entity are NOT overwritten.
    pl.set_function("MakePlayer",
        [&world](uint32_t entityId) -> bool {
            const Entity e = static_cast<Entity>(entityId);
            if (!world.IsAlive(e)) return false;
            if (!world.HasComponent<LocalTransform>(e))
                world.AddComponent<LocalTransform>(e, LocalTransform{});
            if (!world.HasComponent<GlobalTransform>(e))
                world.AddComponent<GlobalTransform>(e, GlobalTransform{});
            if (!world.HasComponent<CharacterControllerComponent>(e))
                world.AddComponent<CharacterControllerComponent>(e, CharacterControllerComponent{});
            if (!world.HasComponent<PlayerComponent>(e))
                world.AddComponent<PlayerComponent>(e, PlayerComponent{});
            return true;
        });

    lua["Player"] = pl;

    // ---- Camera.* ------------------------------------------------------
    // Active-camera switching. App::RefreshMainCamera prefers the entity
    // tagged with ActiveCameraTag; SetActive strips the tag from any prior
    // holder and pins it to the requested entity. Use this for:
    //   * Editor preview cam ↔ play cam swap
    //   * Cutscene push (save current, SetActive new, restore on end)
    //   * Debug fly-cam toggle
    //   * Death-replay / spectator cam
    sol::object existingCam = lua["Camera"];
    sol::table cam = existingCam.is<sol::table>()
                   ? existingCam.as<sol::table>()
                   : lua.create_table();

    cam.set_function("SetActive",
        [&world](uint32_t entityId) -> bool {
            const Entity e = static_cast<Entity>(entityId);
            if (!world.IsAlive(e)) {
                LOG_WARNING("Camera.SetActive: entity %u is not alive", entityId);
                return false;
            }
            if (!world.HasComponent<CameraComponent>(e)) {
                LOG_WARNING("Camera.SetActive: entity %u has no CameraComponent", entityId);
                return false;
            }
            // Strip the tag from any previous holder — at most one active
            // camera at a time. Iterating Entities() instead of ForEach<Tag>()
            // because we're mutating the pool during iteration.
            std::vector<Entity> prevHolders;
            world.ForEach<ActiveCameraTag>([&](Entity other, ActiveCameraTag&)
            {
                if (other != e) prevHolders.push_back(other);
            });
            for (Entity p : prevHolders)
                world.RemoveComponent<ActiveCameraTag>(p);

            if (!world.HasComponent<ActiveCameraTag>(e))
                world.AddComponent<ActiveCameraTag>(e, ActiveCameraTag{});
            return true;
        });

    // Returns the currently-tagged active camera entity, or 0 (NullEntity)
    // if no entity carries the tag. Note: even without a tag, the renderer
    // still uses *some* camera via the first-found fallback — this getter
    // just reports the explicit assignment.
    cam.set_function("GetActive",
        [&world]() -> uint32_t {
            uint32_t active = NullEntity;
            world.ForEach<ActiveCameraTag>([&](Entity e, ActiveCameraTag&)
            {
                if (active == NullEntity) active = static_cast<uint32_t>(e);
            });
            return active;
        });

    // Camera.ClearActive() — drop the tag without picking a new camera.
    // App::RefreshMainCamera then falls back to first-found. Useful at the
    // end of a cutscene before SetActive on the gameplay cam is issued, or
    // when tearing down a scene.
    cam.set_function("ClearActive",
        [&world]() -> bool {
            std::vector<Entity> holders;
            world.ForEach<ActiveCameraTag>([&](Entity e, ActiveCameraTag&)
            {
                holders.push_back(e);
            });
            for (Entity h : holders)
                world.RemoveComponent<ActiveCameraTag>(h);
            return true;
        });

    // ---- Camera Stack (DesignMd/camera_stack_system.md) ----------------
    //
    // Lua interface: Camera.PushVCam / PopVCam / SetPriority / SetWeight /
    //                HardCutTo / AddShake / HashChannel.
    //
    // Cameras are pushed onto a per-channel stack; the highest-priority VCam
    // wins, ties blend by weight. PushVCam takes an optional table:
    //   { priority=100, blendIn=0.4, blendOut=0.3,
    //     curveIn="EaseOut", curveOut="EaseIn",
    //     channel="Main" or 0x...uint }
    auto resolveChannel = [&](const sol::object& chArg) -> CameraChannelId {
        if (chArg.is<std::string>())
            return Camera::HashChannel(chArg.as<std::string>().c_str());
        if (chArg.is<uint32_t>())
            return static_cast<CameraChannelId>(chArg.as<uint32_t>());
        return Camera::kMainChannel;
    };
    auto resolveCurve = [](const sol::object& c) -> BlendCurve {
        if (c.is<std::string>()) {
            const std::string s = c.as<std::string>();
            if (s == "Linear")    return BlendCurve::Linear;
            if (s == "EaseIn")    return BlendCurve::EaseIn;
            if (s == "EaseOut")   return BlendCurve::EaseOut;
            if (s == "EaseInOut" || s == "Smooth") return BlendCurve::EaseInOut;
        }
        return BlendCurve::EaseOut;
    };

    cam.set_function("HashChannel",
        [](const std::string& name) -> uint32_t {
            return Camera::HashChannel(name.c_str());
        });

    cam.set_function("PushVCam",
        [&world, resolveChannel, resolveCurve](uint32_t entityId, sol::object optsObj) -> uint32_t {
            const Entity e = static_cast<Entity>(entityId);
            if (!world.IsAlive(e)) {
                LOG_WARNING("Camera.PushVCam: entity %u not alive", entityId);
                return 0u;
            }
            Camera::PushVCamArgs args{};
            if (optsObj.is<sol::table>())
            {
                sol::table t = optsObj.as<sol::table>();
                args.priority         = t.get_or("priority",  args.priority);
                args.weight           = t.get_or("weight",    args.weight);
                args.blendInDuration  = t.get_or("blendIn",   args.blendInDuration);
                args.blendOutDuration = t.get_or("blendOut",  args.blendOutDuration);
                args.curveIn          = resolveCurve(t["curveIn"]);
                args.curveOut         = resolveCurve(t["curveOut"]);
                args.channelId        = resolveChannel(t["channel"]);
            }
            Camera::PushVCam(world, e, args);
            return entityId;   // handle == entity id
        });

    cam.set_function("PopVCam",
        [&world, resolveCurve](uint32_t entityId, sol::object optsObj) -> bool {
            const Entity e = static_cast<Entity>(entityId);
            if (!world.IsAlive(e)) return false;
            float blendOut = -1.f;
            BlendCurve curve = BlendCurve::Custom; // sentinel = leave alone
            if (optsObj.is<sol::table>())
            {
                sol::table t = optsObj.as<sol::table>();
                blendOut = t.get_or("blendOut", -1.f);
                if (t["curveOut"].valid()) curve = resolveCurve(t["curveOut"]);
            }
            Camera::PopVCam(world, e, blendOut, curve);
            return true;
        });

    cam.set_function("SetPriority",
        [&world](uint32_t entityId, int priority) -> bool {
            const Entity e = static_cast<Entity>(entityId);
            auto* prio = world.GetComponent<VCamPriorityComponent>(e);
            if (!prio) return false;
            prio->priority = priority;
            return true;
        });

    cam.set_function("SetWeight",
        [&world](uint32_t entityId, float w) -> bool {
            const Entity e = static_cast<Entity>(entityId);
            auto* prio = world.GetComponent<VCamPriorityComponent>(e);
            if (!prio) return false;
            prio->weight = w;
            return true;
        });

    cam.set_function("SetEnabled",
        [&world](uint32_t entityId, bool enabled) -> bool {
            const Entity e = static_cast<Entity>(entityId);
            auto* prio = world.GetComponent<VCamPriorityComponent>(e);
            if (!prio) return false;
            prio->enabled = enabled;
            return true;
        });

    cam.set_function("HardCutTo",
        [&world, resolveChannel](uint32_t entityId, sol::object optsObj) -> bool {
            const Entity e = static_cast<Entity>(entityId);
            CameraChannelId ch = Camera::kMainChannel;
            if (optsObj.is<sol::table>())
                ch = resolveChannel(optsObj.as<sol::table>()["channel"]);
            Camera::HardCutTo(world, e, ch);
            return true;
        });

    cam.set_function("AddShake",
        [&world, resolveChannel](float trauma, sol::object optsObj) -> bool {
            CameraChannelId ch = Camera::kMainChannel;
            float falloff = 1.5f;
            if (optsObj.is<sol::table>()) {
                sol::table t = optsObj.as<sol::table>();
                ch = resolveChannel(t["channel"]);
                falloff = t.get_or("falloff", falloff);
            }
            const Entity channelEnt = Camera::GetOrCreateChannelEntity(world, ch);
            if (!world.HasComponent<CameraShakeComponent>(channelEnt))
                world.AddComponent<CameraShakeComponent>(channelEnt, CameraShakeComponent{});
            auto* shake = world.GetComponent<CameraShakeComponent>(channelEnt);
            if (!shake) return false;
            // Trauma stacks additively up to 1.0 — multiple hits in the same
            // frame layer; falloff still drains at the most-recent rate.
            shake->trauma = std::min(1.f, shake->trauma + trauma);
            shake->falloffPerSec = falloff;
            return true;
        });

    lua["Camera"] = cam;
}
