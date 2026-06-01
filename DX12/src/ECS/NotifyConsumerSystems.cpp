#include "ECS/NotifyConsumerSystems.h"

#include "ECS/ECS.h"
#include "ECS/FrameContext.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/FollowComponents.h"
#include "ECS/LifetimeComponent.h"
#include "ECS/NotifyTypes.h"
#include "ECS/ParticleComponent.h"
#include "ECS/TrailComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/Components.h"              // MaterialComponent, MeshHandle, VisibilityComponent, RenderLayer, BoundingVolume
#include "ECS/AnimationComponents.h"     // SocketComponent (bone-attach base pose)
#include "ECS/VFXSpawnRequests.h"        // Lane B mailboxes (Tracer / Decal / Afterimage / Mesh)
#include "ECS/CameraStackComponents.h"   // CameraShakeComponent, Camera::kMainChannel
#include "ECS/CameraStackSystem.h"       // Camera::GetOrCreateChannelEntity

#include "Audio/AudioEvents.h"
#include "Resource/VFXPrefab.h"
#include "System/EventBus.h"
#include "System/Log.h"

#include <algorithm>
#include <DirectXMath.h>
#include <memory>
#include <unordered_map>

// All six systems drain the mailbox they own and clear it. Today most are
// "log + clear" skeletons — the real downstream subsystems (collision
// sweep, VFX asset pipeline, camera shake driver, state stack on the
// character component) wire in as those features land. The Audio path is
// the most concrete: it bridges directly into the existing EventBus-based
// AudioSystem so notifies start producing sound immediately.

void HitboxSystem::Update(World& world, const FrameContext&)
{
    world.ForEach<PendingHitboxCommands>([&](Entity e, PendingHitboxCommands& mb) {
        for (const auto& cmd : mb.commands)
        {
            // TODO: route to collision-sweep / damage subsystem when it lands.
            // For now: visible log so the timeline editor's preview clearly
            // shows "yes, the notify fired on entity X".
            LOG_INFO("[Hitbox] entity=%u slot=%u type=%d radius=%.2f dmg=%.1f group=%u",
                e, cmd.slot, static_cast<int>(cmd.type),
                cmd.params.radius, cmd.params.damage, cmd.params.hitGroup);
        }
        mb.commands.clear();
    });
}

namespace
{
    // Process-lifetime cache. Keyed by asset path; the prefab itself is
    // tiny (a vector of particle params) so we keep loaded prefabs forever
    // — there's no point ref-counting something that fits in a few KB.
    // shared_ptr lets a future hot-reload path swap the contents safely
    // without invalidating in-flight spawns.
    const Resource::VFXPrefab* GetOrLoadVFXPrefab(const std::string& path)
    {
        static std::unordered_map<std::string, std::shared_ptr<Resource::VFXPrefab>> s_cache;
        auto it = s_cache.find(path);
        if (it != s_cache.end()) return it->second.get();

        auto prefab = std::make_shared<Resource::VFXPrefab>();
        if (!Resource::LoadVFXPrefab(path, *prefab)) {
            // Cache the failed result too — re-attempting the same broken
            // path 60×/sec while a designer fixes the file would spam logs.
            s_cache.emplace(path, prefab);
            return prefab.get();
        }
        auto inserted = s_cache.emplace(path, std::move(prefab));
        return inserted.first->second.get();
    }

    // Compose two LocalTransforms (Spawn-cmd offset × prefab emitter local).
    // Caller invariant: scale stays multiplicative, translation is rotated
    // by parent rotation. This matches how TransformSystem propagates from
    // Parent → child, so the spawned emitter ends up where a Parent/Children
    // hierarchy would have placed it without us having to actually rig one.
    LocalTransform Compose(const LocalTransform& a, const LocalTransform& b)
    {
        using namespace DirectX;
        const XMMATRIX m = b.ToMatrix() * a.ToMatrix();
        XMVECTOR s, r, t;
        XMMatrixDecompose(&s, &r, &t, m);

        LocalTransform out;
        XMStoreFloat3(&out.translation, t);
        XMStoreFloat4(&out.rotation,    r);
        XMStoreFloat3(&out.scale,       s);
        return out;
    }

    // Bake a world-space matrix into a LocalTransform (Detached mode —
    // the spawned emitter is parented to nothing, so its "local" is
    // really world-space).
    LocalTransform BakeFromMatrix(const DirectX::XMFLOAT4X4& world,
                                  const LocalTransform&      prefabLocal)
    {
        using namespace DirectX;
        const XMMATRIX m = prefabLocal.ToMatrix() * XMLoadFloat4x4(&world);
        XMVECTOR s, r, t;
        XMMatrixDecompose(&s, &r, &t, m);

        LocalTransform out;
        XMStoreFloat3(&out.translation, t);
        XMStoreFloat4(&out.rotation,    r);
        XMStoreFloat3(&out.scale,       s);
        return out;
    }

    // Push a fully-resolved Lane-B request onto the spawner's mailbox (get or
    // create the component). Renderer::BeginFrame drains it the same frame.
    template <typename MB, typename Req>
    void PushReq(World& world, Entity owner, Req&& r)
    {
        auto* mb = world.GetComponent<MB>(owner);
        if (!mb) { world.AddComponent<MB>(owner, {}); mb = world.GetComponent<MB>(owner); }
        if (mb) mb->reqs.push_back(std::forward<Req>(r));
    }

    // Best-known world pose to attach a VFX instance to. For bone-attach we use
    // the spawner's live socket transform (already refreshed this frame in the
    // Animation phase) so the instance — a trail especially — samples from the
    // right place on its first frame instead of the origin. FollowEntity /
    // Detached fall back to the spawner's root world.
    DirectX::XMFLOAT4X4 GetAttachBaseWorld(World& world, Entity spawner,
                                           const PendingVFXSpawns::Spawn& s)
    {
        DirectX::XMFLOAT4X4 base;
        DirectX::XMStoreFloat4x4(&base, DirectX::XMMatrixIdentity());
        if (s.mode == VFXAttachMode::AttachToBone)
            if (auto* sc = world.GetComponent<SocketComponent>(spawner))
            {
                const uint32_t idx = (s.boneIndex >= 0) ? static_cast<uint32_t>(s.boneIndex) : 0u;
                if (idx < sc->count) return sc->sockets[idx].worldTransform;
            }
        if (auto* gt = world.GetComponent<GlobalTransform>(spawner))
            base = gt->matrix;
        return base;
    }

    // Shared Lane-A spawn path. Creates the instance entity, lets the caller
    // attach the type-specific backend component(s), wires attach mode +
    // lifetime, and seeds GlobalTransform from the spawn pose. Returns the
    // instance. Every component-driven VFX type reuses this so the attach /
    // lifetime policy lives in exactly one place.
    template <typename AddBackend>
    Entity SpawnEntityVFX(World& world, Entity spawner,
                          const PendingVFXSpawns::Spawn&   s,
                          const LocalTransform&            cmdOffset,
                          const Resource::VFXEmitterSpec&  spec,
                          const DirectX::XMFLOAT4X4&       baseWorld,
                          float duration, AddBackend&& addBackend)
    {
        using namespace DirectX;
        const Entity inst = world.CreateEntity();
        addBackend(inst);

        const LocalTransform composed = Compose(cmdOffset, spec.localTransform);

        // Seed GlobalTransform with the spawn pose — FollowSystem / Transform
        // only refresh it next frame, and a zero matrix would flash the VFX at
        // the origin (and make a trail streak in from world-zero).
        GlobalTransform gt;
        XMStoreFloat4x4(&gt.matrix, composed.ToMatrix() * XMLoadFloat4x4(&baseWorld));
        world.AddComponent<GlobalTransform>(inst, gt);

        if (duration > 0.f) {
            LifetimeComponent lt; lt.remaining = duration; lt.total = duration;
            world.AddComponent<LifetimeComponent>(inst, lt);
        }

        switch (s.mode)
        {
        case VFXAttachMode::AttachToBone:
        {
            FollowSocketComponent fs;
            fs.target      = world.MakeHandle(spawner);
            fs.socketIndex = (s.boneIndex >= 0) ? static_cast<uint32_t>(s.boneIndex) : 0u;
            XMStoreFloat4x4(&fs.localOffset, composed.ToMatrix());
            world.AddComponent<FollowSocketComponent>(inst, fs);
            world.AddComponent<LocalTransform>(inst, LocalTransform{});
            break;
        }
        case VFXAttachMode::FollowEntity:
        {
            FollowEntityComponent fe;
            fe.target = world.MakeHandle(spawner);
            XMStoreFloat4x4(&fe.localOffset, composed.ToMatrix());
            world.AddComponent<FollowEntityComponent>(inst, fe);
            world.AddComponent<LocalTransform>(inst, LocalTransform{});
            break;
        }
        case VFXAttachMode::Detached:
        default:
            world.AddComponent<LocalTransform>(inst, BakeFromMatrix(baseWorld, composed));
            break;
        }
        return inst;
    }
}

void VFXSpawnSystem::Update(World& world, const FrameContext&)
{
    // Collect first, then spawn. ForEach holds the PendingVFXSpawns pool
    // view; AddComponent / CreateEntity inside the callback would corrupt
    // iteration when the new entity itself also gets a PendingVFXSpawns
    // somewhere down the line.
    struct Pending {
        Entity                       spawner;
        PendingVFXSpawns::Spawn      spawn;
    };
    std::vector<Pending> queue;

    world.ForEach<PendingVFXSpawns>([&](Entity e, PendingVFXSpawns& mb) {
        for (auto& s : mb.spawns) queue.push_back({ e, std::move(s) });
        mb.spawns.clear();
    });

    for (const auto& q : queue)
    {
        const Entity spawner = q.spawner;
        const auto&  s       = q.spawn;

        if (s.assetPath.empty()) continue;
        const auto* prefab = GetOrLoadVFXPrefab(s.assetPath);
        if (!prefab || prefab->emitters.empty()) continue;

        // Best-known attach base pose (socket world for bone-attach, else
        // root world) — shared by every emitter in this spawn command.
        const DirectX::XMFLOAT4X4 baseWorld = GetAttachBaseWorld(world, spawner, s);

        // Spawn-cmd-level local offset (designer-authored at the notify
        // params level); composed over each prefab emitter's own offset.
        LocalTransform cmdOffset;
        cmdOffset.translation = s.localPos;

        for (const Resource::VFXEmitterSpec& spec : prefab->emitters)
        {
            // Per-emitter effective lifetime: explicit override > spawn-cmd
            // duration > prefab default. (<= 0 means "no LifetimeComponent".)
            const float dur =
                (spec.durationOverride >= 0.f) ? spec.durationOverride :
                (s.duration > 0.f)             ? s.duration :
                                                 prefab->defaultDuration;

            // NOTE: spec.startDelay (staggered spawn) is parsed but not yet
            // honoured — deferred activation is the Phase 5 work item, so all
            // emitters currently spawn together (simultaneous coordination).

            switch (spec.type)
            {
            // ---- Lane A: component-driven, pure ECS ----------------------
            case Resource::VFXEmitterType::Particle:
                // ParticleSystem resolves texturePath's bindless slot on its
                // first tick after the component appears.
                SpawnEntityVFX(world, spawner, s, cmdOffset, spec, baseWorld, dur,
                    [&](Entity inst){ world.AddComponent<ParticleEmitterComponent>(inst, spec.particle); });
                break;

            case Resource::VFXEmitterType::Trail:
                // Trail samples its GlobalTransform translation each frame, so
                // it only makes sense on a moving (Attach/Follow) instance —
                // the seeded GlobalTransform keeps the first segment off the origin.
                SpawnEntityVFX(world, spawner, s, cmdOffset, spec, baseWorld, dur,
                    [&](Entity inst){ world.AddComponent<TrailComponent>(inst, spec.trail); });
                break;

            case Resource::VFXEmitterType::Beam:
                // Beam control points are authored in local space; the entity's
                // (attached) world matrix transforms them in the VS. The custom
                // PS + blend mode come from the prefab.
                SpawnEntityVFX(world, spawner, s, cmdOffset, spec, baseWorld, dur,
                    [&](Entity inst){
                        world.AddComponent<BeamComponent>(inst, spec.beam.beam);
                        MaterialComponent mc;
                        mc.useCustomShader  = true;
                        mc.customShaderPath = spec.beam.shaderPath;
                        mc.userBlendMode    = spec.beam.additive ? BlendMode::Additive : BlendMode::Opaque;
                        world.AddComponent<MaterialComponent>(inst, std::move(mc));
                    });
                break;

            // ---- Lane B: Renderer-owned backends via mailbox -------------
            case Resource::VFXEmitterType::Tracer:
            {
                using namespace DirectX;
                const LocalTransform composed = Compose(cmdOffset, spec.localTransform);
                const XMMATRIX worldM = composed.ToMatrix() * XMLoadFloat4x4(&baseWorld);
                XMFLOAT4X4 wf; XMStoreFloat4x4(&wf, worldM);

                PendingTracerSpawns::Req r;
                r.start = { wf._41, wf._42, wf._43 };
                XMVECTOR dirW = XMVector3Normalize(
                    XMVector3TransformNormal(XMLoadFloat3(&spec.tracer.direction), worldM));
                XMStoreFloat3(&r.end, XMLoadFloat3(&r.start) + dirW * spec.tracer.length);
                r.color            = spec.tracer.color;
                r.width            = spec.tracer.width;
                r.lifetime         = spec.tracer.lifetime;
                r.noiseTexBindless = spec.tracer.noiseTexBindless;
                PushReq<PendingTracerSpawns>(world, spawner, std::move(r));
                break;
            }

            case Resource::VFXEmitterType::Decal:
            {
                using namespace DirectX;
                const LocalTransform composed = Compose(cmdOffset, spec.localTransform);
                const XMMATRIX worldM = composed.ToMatrix() * XMLoadFloat4x4(&baseWorld);
                XMFLOAT4X4 wf; XMStoreFloat4x4(&wf, worldM);

                PendingDecalSpawns::Req r;
                r.materialName    = spec.decal.materialName;
                r.position        = { wf._41, wf._42, wf._43 };
                XMStoreFloat3(&r.normal, XMVector3Normalize(XMVectorSet(wf._31, wf._32, wf._33, 0.f)));
                r.size            = { spec.decal.sizeX, spec.decal.sizeY };
                r.depth           = spec.decal.depth;
                r.lifetime        = (spec.durationOverride >= 0.f) ? spec.durationOverride : spec.decal.lifetime;
                r.fadeOutDuration = spec.decal.fadeOutDuration;
                r.rollZ           = spec.decal.rollZ;
                r.tintOverride    = spec.decal.tintOverride;
                PushReq<PendingDecalSpawns>(world, spawner, std::move(r));
                break;
            }

            case Resource::VFXEmitterType::Afterimage:
            {
                // Snapshots the SPAWNER character itself — no instance entity.
                PendingAfterimageSpawns::Req r;
                r.target   = world.MakeHandle(spawner);
                r.lifetime = (spec.durationOverride >= 0.f) ? spec.durationOverride : spec.afterimage.lifetime;
                r.color    = spec.afterimage.color;
                PushReq<PendingAfterimageSpawns>(world, spawner, std::move(r));
                break;
            }

            // ---- Mesh: hybrid — Lane-A entity, Lane-B GPU mesh resolve ----
            case Resource::VFXEmitterType::Mesh:
            {
                const Entity inst = SpawnEntityVFX(world, spawner, s, cmdOffset, spec, baseWorld, dur,
                    [&](Entity e){
                        world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
                        world.AddComponent<RenderLayer>(e, RenderLayer{});
                        world.AddComponent<BoundingVolume>(e, BoundingVolume{});
                        world.AddComponent<MaterialComponent>(e, MaterialComponent{});
                        if (!spec.mesh.materialPath.empty())
                            world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ spec.mesh.materialPath });
                        world.AddComponent<MeshSourcePath>(e, MeshSourcePath{ spec.mesh.meshPath });
                    });
                PendingMeshVFXSpawns::Req r;
                r.entity       = world.MakeHandle(inst);
                r.meshPath     = spec.mesh.meshPath;
                r.materialPath = spec.mesh.materialPath;
                r.castShadow   = spec.mesh.castShadow;
                PushReq<PendingMeshVFXSpawns>(world, spawner, std::move(r));
                break;
            }

            default: break;
            }
        }
    }
}

void CameraEffectSystem::Update(World& world, const FrameContext&)
{
    world.ForEach<PendingCameraEffects>([&](Entity e, PendingCameraEffects& mb) {
        for (const auto& ef : mb.effects)
        {
            switch (ef.type)
            {
            case CameraEffectType::Shake:
            {
                // Route into the same trauma-based shake the Lua Camera.AddShake
                // API drives. CameraShakeComponent lives on the Main channel's
                // LiveCamera entity; CameraShakeTickSystem (PreRender phase, i.e.
                // AFTER this BoneAttachment phase) turns trauma² into additive
                // pos/rot noise on the resolved pose — same frame as the notify.
                const Entity ch = Camera::GetOrCreateChannelEntity(world, Camera::kMainChannel);
                if (!world.HasComponent<CameraShakeComponent>(ch))
                    world.AddComponent<CameraShakeComponent>(ch, CameraShakeComponent{});
                if (auto* shake = world.GetComponent<CameraShakeComponent>(ch))
                {
                    // The notify "amp" is treated as trauma (0..1); it stacks
                    // additively so multiple hits in one frame layer up.
                    const float trauma = std::clamp(ef.params.shakeAmplitude, 0.f, 1.f);
                    shake->trauma = std::min(1.f, shake->trauma + trauma);
                    if (ef.params.shakeFrequency > 0.f)
                        shake->frequency = ef.params.shakeFrequency;
                    // Drain so the shake lasts roughly `duration` seconds.
                    shake->falloffPerSec = (ef.params.duration > 0.01f)
                        ? std::max(0.1f, trauma / ef.params.duration)
                        : 1.5f;
                }
            } break;

            case CameraEffectType::Zoom:
            case CameraEffectType::FOVPunch:
                // Not yet wired: CameraResolveSystem overwrites LiveCamera.fov
                // every frame, so a one-shot nudge here would be wiped. The
                // clean fix mirrors the shake layer — a persistent additive-FOV
                // component + a small decay system applied post-resolve.
                LOG_INFO("[Camera] FOV effect kind=%d fovDelta=%.2f dur=%.2f — not yet wired "
                         "(needs an additive-FOV layer like CameraShakeComponent)",
                         static_cast<int>(ef.type), ef.params.fovDelta, ef.params.duration);
                break;

            case CameraEffectType::HitStop:
                // Not yet wired: hit-stop must scale gameplay/physics dt, which
                // flows through ScriptSystem::GetTimeScale → FrameContext::
                // scaledDeltaTime. Needs a global time-scale service that holds
                // the scale for `duration` then restores it.
                LOG_INFO("[Camera] HitStop scale=%.2f dur=%.2f — not yet wired "
                         "(needs a timed global time-scale service)",
                         ef.params.timeScale, ef.params.duration);
                break;
            }
        }
        mb.effects.clear();
    });
}

void AudioPlaySystem::Update(World& world, const FrameContext&)
{
    auto& bus = EventBus::Get();

    world.ForEach<PendingAudioPlays>([&](Entity e, PendingAudioPlays& mb) {
        for (const auto& p : mb.plays)
        {
            // Bridge to the existing audio path: publish a PlaySoundEvent
            // the AudioSystem already subscribes to. We do NOT acquire the
            // VoiceHandle here — fire-and-forget is the right default for
            // animation-driven SFX.
            Audio::PlaySoundEvent ev;
            ev.entity   = EntityHandle{ e, 0u };  // generation unused here
            ev.clipPath = p.clipPath;
            ev.volume   = p.volume;
            ev.pitch    = p.pitch;
            switch (p.category) {
                case AudioCategoryTag::SFX:   ev.bus = Audio::BusType::SFX;   break;
                case AudioCategoryTag::Voice: ev.bus = Audio::BusType::Voice; break;
                case AudioCategoryTag::Foley: ev.bus = Audio::BusType::SFX;   break;
                case AudioCategoryTag::Music: ev.bus = Audio::BusType::Music; break;
            }
            ev.is3D = (p.attachBone >= 0);
            bus.Publish<Audio::PlaySoundEvent>(std::move(ev));
        }
        mb.plays.clear();
    });
}

void StateToggleSystem::Update(World& world, const FrameContext&)
{
    world.ForEach<PendingStateToggles>([&](Entity e, PendingStateToggles& mb) {
        for (const auto& t : mb.toggles)
        {
            // TODO: write into a CharacterStateComponent tag set (iframe /
            // super armor / input-buffer window). Lives in gameplay code
            // because the tags themselves are gameplay-defined.
            LOG_INFO("[StateToggle] entity=%u tag=%u enable=%d dur=%.2f",
                e, static_cast<uint32_t>(t.tag), t.enable ? 1 : 0, t.duration);
        }
        mb.toggles.clear();
    });
}

// CustomNotifyEvent — published by GenericNotifyDispatcher so Lua / script
// subscribers can listen for designer-authored events without needing a C++
// component for each tag. Defined here (not in a public header) because the
// only producer is GenericNotifyDispatcher and the only intended consumer is
// the script layer, which subscribes via the same EventBus.
struct CustomNotifyEvent
{
    Entity      entity = NullEntity;
    StringID    typeId;
    PropertyBag params;
};

void GenericNotifyDispatcher::Update(World& world, const FrameContext&)
{
    auto& bus = EventBus::Get();
    world.ForEach<PendingGenericNotifies>([&](Entity e, PendingGenericNotifies& mb) {
        for (auto& n : mb.notifies) {
            CustomNotifyEvent ev;
            ev.entity = e;
            ev.typeId = std::move(n.typeId);
            ev.params = std::move(n.params);
            bus.Publish<CustomNotifyEvent>(std::move(ev));
        }
        mb.notifies.clear();
    });
}
