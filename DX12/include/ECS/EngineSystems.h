#pragma once

// EngineSystems.h — thin ISystem adapters that wrap today's concrete
// per-engine singletons (ScriptSystem, AISystem, PhysicsSystem, ...)
// so they can be driven by Scheduler instead of being called by hand
// inside App::Run.
//
// Each adapter is a 1-to-1 wrapper around an existing system's
// per-frame call(s). Adapters take references to the singletons via
// their constructor; ownership stays with App. See
// SystemRegistry::RegisterAllSystems in SystemRegistry.cpp for the
// single tick-order declaration point.
//
// PHASE MAPPING NOTES (preserves existing tick semantics):
//   * Script::TickTimers   → GameplayPreLogic
//   * Script::Update + CheckHotReload → GameplayLogic
//   * AI LOD / BT / NavAgent / PlayerController → AI
//     (Player is the LAST AI system; matches the historical
//      "Script → AI → NavAgent → Player → Physics" order — design
//      doc's GameplayPostLogic phase runs BEFORE AI which doesn't
//      match the current dependency direction, so player rides AI.
//      NavAgent writes desiredHorizontalVelocity on
//      CharacterControllerComponent, same field PlayerController writes
//      to — KCC step in FixedPhysics consumes both uniformly.)
//   * GameModeStack::Update + pending transition → GameplayPostLogic
//   * PhysicsSystem::Update → FixedPhysics
//     (PhysicsSystem still owns its INTERNAL 60Hz accumulator in S1;
//      the FixedPhysicsPre/Post phase shells stay empty until we
//      externalise the accumulator in a follow-up.)
//   * TransformSystem::Propagate → PhysicsInterpolation (always-on)
//   * PhysicsSystem::ApplyRenderInterpolation → PhysicsInterpolation
//     (gated on runUpdate to preserve editor Stopped behaviour)
//   * CameraSystem::ResolveFollowing → PhysicsInterpolation
//   * Audio block (clip Tick + EventBus drains + AudioSystem +
//     Audio3DSystem + AudioEngine) → PreRender
//     (audio output isn't render-coupled, but PreRender is post-
//      physics-interpolation so collision SFX still fire same frame
//      as the collision that produced them.)

#include "ISystem.h"

#include <functional>

// LambdaSystem — generic Phase-typed adapter that just invokes a callback.
//
// Used for systems whose dependencies are large/transient enough that
// turning them into a real class + services struct adds more boilerplate
// than abstraction value. The canonical client is the App's inline render
// block: it touches ~15 App-locals + App-members, but its only contract
// with the scheduler is "run me once in the Render phase". The lambda
// captures App's locals/members by reference; the LambdaSystem is owned
// by SystemRegistry, which is destroyed in App's destructor BEFORE the
// captured locals (App::Run's stack) go out of scope, so by-ref capture
// is safe so long as the registry is shut down inside Run() before
// returning (which App::Run does explicitly).
template <TickPhase PHASE>
class LambdaSystem : public SystemInPhase<PHASE>
{
public:
    using Fn = std::function<void(World&, const FrameContext&)>;
    LambdaSystem(const char* name, Fn fn)
        : m_name(name), m_fn(std::move(fn)) {}
    void Update(World& world, const FrameContext& ctx) override
    {
        m_fn(world, ctx);
    }
    const char* GetName() const override { return m_name; }
private:
    const char* m_name;
    Fn          m_fn;
};

// Render-phase alias — App::Run constructs this with the inline render
// block as its lambda body.
using RenderSystem = LambdaSystem<TickPhase::Render>;

// ---- Forward decls of the concrete singletons --------------------------
class World;
class Renderer;
class ScriptSystem;
class PlayerControllerSystem;
class CameraSystem;
class GameModeStack;
namespace AI       { class AISystem; class AILODSystem; }
namespace Nav      { class NavMeshSystem; }
namespace DX12Physics { class PhysicsSystem; }
namespace Audio    {
    class AudioClipSystem;
    class AudioSystem;
    class Audio3DSystem;
    class AudioEngine;
}

// ===== GameplayPreLogic =================================================

class ScriptTimerSystem : public SystemInPhase<TickPhase::GameplayPreLogic>
{
public:
    explicit ScriptTimerSystem(ScriptSystem& s) : m_script(s) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "ScriptTimerSystem"; }
private:
    ScriptSystem& m_script;
};

// ===== GameplayLogic ====================================================

class ScriptLogicSystem : public SystemInPhase<TickPhase::GameplayLogic>
{
public:
    explicit ScriptLogicSystem(ScriptSystem& s) : m_script(s) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "ScriptLogicSystem"; }
private:
    ScriptSystem& m_script;
};

// ===== AI ===============================================================

class AILODTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    AILODTickSystem(AI::AILODSystem& s) : m_lod(s) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "AILODSystem"; }
private:
    AI::AILODSystem& m_lod;
};

class AIBTTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    AIBTTickSystem(AI::AISystem& s) : m_ai(s) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "AIBTSystem"; }
private:
    AI::AISystem& m_ai;
};

class AITacticalTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    AITacticalTickSystem() = default;
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "AITacticalSystem"; }
};

class NavAgentTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    explicit NavAgentTickSystem(Nav::NavMeshSystem& nav) : m_nav(nav) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "NavAgentSystem"; }
private:
    Nav::NavMeshSystem& m_nav;
};

class PlayerControlTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    PlayerControlTickSystem(PlayerControllerSystem& pc) : m_pc(pc) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "PlayerControllerSystem"; }
private:
    PlayerControllerSystem& m_pc;
};

// ===== GameplayPostLogic ================================================

class GameModeStackTickSystem : public SystemInPhase<TickPhase::GameplayPostLogic>
{
public:
    GameModeStackTickSystem(GameModeStack& s) : m_modes(s) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "GameModeStackTickSystem"; }
private:
    GameModeStack& m_modes;
};

// ===== FixedPhysics =====================================================

class PhysicsStepSystem : public SystemInPhase<TickPhase::FixedPhysics>
{
public:
    PhysicsStepSystem(DX12Physics::PhysicsSystem& p) : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "PhysicsStepSystem"; }
private:
    DX12Physics::PhysicsSystem& m_physics;
};

// ===== PhysicsInterpolation =============================================

class TransformPropagateSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "TransformPropagateSystem"; }
};

class PhysicsInterpApplySystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    PhysicsInterpApplySystem(DX12Physics::PhysicsSystem& p) : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "PhysicsInterpolation"; }
private:
    DX12Physics::PhysicsSystem& m_physics;
};

class CameraFollowResolveSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    CameraFollowResolveSystem(DX12Physics::PhysicsSystem& p) : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "CameraFollowResolveSystem"; }
private:
    DX12Physics::PhysicsSystem& m_physics;
};

// ===== Animation ========================================================
// Wraps Renderer::TickAnimationChain (extracted from Renderer::BeginFrame).
// Runs the entire character-state → AnimationSystem sample → IK →
// ChainPhysics → LocalToWorld → Socket/Follow → bone AABB merge →
// SkinMatrix → BuildSkinJobs pipeline. Always-on (no runUpdate gate) so
// turntable / preview animation keeps playing while gameplay is paused.

class RendererAnimationChainSystem
    : public SystemInPhase<TickPhase::Animation>
{
public:
    RendererAnimationChainSystem(Renderer& r) : m_renderer(r) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "RendererAnimationChain"; }
private:
    Renderer& m_renderer;
};

// ===== PreRender ========================================================

class AudioTickSystem : public SystemInPhase<TickPhase::PreRender>
{
public:
    AudioTickSystem(Audio::AudioClipSystem& clip,
                    Audio::AudioSystem&     audio,
                    Audio::Audio3DSystem&   audio3D,
                    Audio::AudioEngine&     engine)
        : m_clip(clip), m_audio(audio), m_audio3D(audio3D), m_engine(engine) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "AudioTickSystem"; }
private:
    Audio::AudioClipSystem& m_clip;
    Audio::AudioSystem&     m_audio;
    Audio::Audio3DSystem&   m_audio3D;
    Audio::AudioEngine&     m_engine;
};
