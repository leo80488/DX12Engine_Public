#include "ECS/EngineSystems.h"
#include "ECS/FrameContext.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/CameraSystem.h"
#include "ECS/PlayerControllerSystem.h"
#include "ECS/PlayerComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "Scripting/ScriptSystem.h"
#include "AI/AISystem.h"
#include "AI/AILODSystem.h"
#include "AI/AIComponents.h"
#include "Nav/NavMeshSystem.h"
#include "Nav/NavAgentSystem.h"
#include "Nav/NavComponents.h"
#include "ECS/AIIntentComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "AI/AITacticalSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/GameModeStack.h"
#include "Scene/TransformSystem.h"
#include "Audio/AudioClipSystem.h"
#include "Audio/AudioSystem.h"
#include "Audio/Audio3DSystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioEvents.h"
#include "System/EventBus.h"
#include "Graphics/Renderer.h"

// ===== GameplayPreLogic =================================================

void ScriptTimerSystem::Update(World& /*world*/, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    // Real (unscaled) dt — hit-stop callbacks must expire even when
    // m_timeScale ≈ 0 freezes gameplay.
    m_script.TickTimers(ctx.deltaTime);
}

// ===== GameplayLogic ====================================================

void ScriptLogicSystem::Update(World& world, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    m_script.Update(world, ctx.scaledDeltaTime);
    // Hot-reload check follows Update to mirror the historical inline
    // order in App::Run (line 532 pre-refactor).
    m_script.CheckHotReload(world);
}

// ===== AI ===============================================================

void AILODTickSystem::Update(World& world, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    m_lod.SetCameraEntity(static_cast<Entity>(ctx.cameraEntity));
    m_lod.Update(world);
}

void AILODTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Reads agent world poses (distance-from-camera), writes tickInterval.
    b.Read<GlobalTransform>()
     .Write<AIComponent>();
}

void AIBTTickSystem::Update(World& world, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    m_ai.CheckHotReload(world);
    m_ai.Update(world, ctx.scaledDeltaTime);
}

void AITacticalTickSystem::Update(World& world, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    AI::AITacticalTick(world, ctx.scaledDeltaTime);
}

void AITacticalTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // AIIntent is the strategic input (BT wrote it earlier in this phase),
    // we translate into NavAgent fields (destination, facingMode, etc.).
    b.Read<AIIntentComponent>()
     .Write<NavAgentComponent>()
     .Read<GlobalTransform>();   // for resolving target entity positions
}

void NavAgentTickSystem::Update(World& world, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    Nav::NavAgentTick(world, m_nav, ctx.scaledDeltaTime);
}

void NavAgentTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // NavAgent holds the path runtime state we mutate; LocalTransform's
    // rotation is the output (translation is written by KCC post-step,
    // not by us), CharacterControllerComponent.desiredHorizontalVelocity
    // is our steering output. NavMesh queries are read-only against the
    // baked tile data.
    b.Read<NavAgentComponent>()
     .Write<NavAgentComponent>()
     .Write<CharacterControllerComponent>()
     .Write<LocalTransform>();
}

void PlayerControlTickSystem::Update(World& world, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    // mainCamera fallback used by PlayerComponent.cameraEntity == NullEntity.
    m_pc.Update(world, static_cast<Entity>(ctx.cameraEntity),
                ctx.scaledDeltaTime);
}

void PlayerControlTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Reads PlayerComponent + camera basis from GlobalTransform; writes
    // drive fields on CharacterControllerComponent. Input::Get() is a
    // per-frame snapshot taken before the scheduler runs — read-only and
    // safe to touch from any AI-phase worker.
    b.Read<PlayerComponent>()
     .Read<GlobalTransform>()
     .Write<CharacterControllerComponent>();
}

// ===== GameplayPostLogic ================================================

void GameModeStackTickSystem::Update(World& /*world*/, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    m_modes.Update(ctx.scaledDeltaTime);
    // Pending mode transitions are drained by the caller (App::Run)
    // because PushMode wants the live RenderContext. Not modelled here.
}

// ===== FixedPhysics =====================================================

void PhysicsStepSystem::Update(World& world, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    // Driven by App's external accumulator: PhysicsStepSystem fires once
    // per RunPhase(FixedPhysics) invocation, which the main loop wraps in
    // while(accumulator >= fixedDt). PhysicsSystem::PreAllSteps /
    // PostAllSteps are called inline in App::Run around the substep loop.
    m_physics.StepOnce(world);
}

// ===== PhysicsInterpolation =============================================

void TransformPropagateSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    // Always-on (no runUpdate gate). The editor gizmo writes
    // LocalTransform and immediately patches ITS GlobalTransform; only
    // this pass propagates the change to descendants. Skipping while
    // paused leaves children visually pinned to stale world positions.
    TransformSystem::Propagate(world);
}

void PhysicsInterpApplySystem::Update(World& world, const FrameContext& ctx)
{
    // Gated on runUpdate: while Stopped, prev == curr in every snapshot
    // anyway, but the overwrite would still stomp on gizmo drags.
    if (!ctx.runUpdate) return;
    // physicsAlpha = App's accumulator / fixedDt — populated after the
    // FixedPhysics substep loop completes.
    m_physics.ApplyRenderInterpolation(world, ctx.physicsAlpha);
}

void CameraFollowResolveSystem::Update(World& world, const FrameContext& ctx)
{
    const Entity camEnt = static_cast<Entity>(ctx.cameraEntity);
    if (camEnt == NullEntity) return;

    auto* ctrl = world.GetComponent<CameraControllerComponent>(camEnt);
    if (!ctrl) return;
    if (ctrl->mode == CameraControllerComponent::Mode::Free) return;

    auto* lt = world.GetComponent<LocalTransform>(camEnt);
    if (!lt) return;

    // ResolveFollowing reads the followTarget's freshly-propagated world
    // pose — that's why this system lives AFTER TransformPropagate in the
    // same phase.
    //
    // dt drives the ThirdPerson follow smoothing. Use scaled gameplay time so
    // the camera lag freezes consistently with the (time-scaled) target it
    // tracks. When the sim isn't running (editor Stopped/Paused) pass 0 so the
    // camera snaps to the target instead of gliding — this system is NOT
    // runUpdate-gated, so without this it would drift toward a gizmo-dragged
    // target while paused.
    const float followDt = ctx.runUpdate ? ctx.scaledDeltaTime : 0.0f;
    CameraSystem::ResolveFollowing(world, *ctrl, *lt, followDt, &m_physics);

    // Camera is a root entity → its world matrix is just its local matrix.
    // Patch GlobalTransform inline so this same frame's RenderSystem sees
    // the resolved pose without a second Propagate pass.
    if (auto* gt = world.GetComponent<GlobalTransform>(camEnt))
        DirectX::XMStoreFloat4x4(&gt->matrix, lt->ToMatrix());
}

// ===== Animation ========================================================

void RendererAnimationChainSystem::Update(World& world, const FrameContext& ctx)
{
    // The chain itself is ALWAYS run (skinning/pose buffers must be rebuilt for
    // this frame's GPU slot every frame, or a frozen character renders with a
    // stale/invalid pose slot). What we gate is only the *time advance*:
    //
    //   animDt = runUpdate ? deltaTime : 0
    //
    // so in the editor, animation playback follows the Start/Pause/Stop state
    // (runUpdate is false while Stopped/Paused, true while Playing or on a
    // single Step where deltaTime is 1/60). In a Game build runUpdate is always
    // true, so animation always plays. AnimationSystem::Update samples the clip
    // every call regardless of dt and only advances primaryTime when dt>0, so
    // dt=0 freezes the pose in place while still rendering it correctly.
    //
    // dt stays REAL (unscaled) deltaTime, NOT scaledDeltaTime: animation should
    // keep playing through a ScriptSystem.SetTimeScale(0) gameplay hit-stop —
    // that is a gameplay-time freeze, distinct from the editor play-state gate.
    const float animDt = ctx.runUpdate ? ctx.deltaTime : 0.0f;
    m_renderer.TickAnimationChain(world, ctx.frameIndex, animDt);
}

// ===== PreRender ========================================================

void AudioTickSystem::Update(World& world, const FrameContext& ctx)
{
    if (!ctx.runUpdate) return;
    // Promote RM-loaded .aclip resources to local Ready FIRST so events
    // / playOnEnable that landed this frame can resolve.
    m_clip.Tick();

    EventBus::Get().DispatchOne<Audio::PlaySoundEvent>();
    EventBus::Get().DispatchOne<Audio::StopSoundEvent>();
    EventBus::Get().DispatchOne<Audio::SetAudioParamEvent>();
    EventBus::Get().DispatchOne<Audio::BusVolumeChangedEvent>();

    m_audio  .Update(world, ctx.scaledDeltaTime);
    m_audio3D.Update(world, ctx.scaledDeltaTime);
    m_engine .Update(ctx.scaledDeltaTime);
}
