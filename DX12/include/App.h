#pragma once

#include "System/Window.h"
#ifdef WITH_EDITOR
#include "Editor/EditorLayer.h"
#endif
#include "Scene/GameModeStack.h"

// ---- Resource systems --------------------------------------------------
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "Resource/MeshSystem.h"
#include "Resource/MeshLibrary.h"
#include "Resource/MaterialSystem.h"
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"

// ---- ECS + scheduler ---------------------------------------------------
#include "ECS/ECS.h"
#include "ECS/CameraSystem.h"
#include "ECS/PlayerControllerSystem.h"
#include "ECS/SystemRegistry.h"
#include "ECS/Scheduler.h"
#include "ECS/CommandBuffer.h"

// ---- Gameplay subsystems ----------------------------------------------
#include "Scripting/ScriptSystem.h"
#include "AI/AISystem.h"
#include "AI/AILODSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Nav/NavMeshSystem.h"

// ---- Audio -------------------------------------------------------------
#include "Audio/AudioEngine.h"
#include "Audio/AudioSystem.h"
#include "Audio/Audio3DSystem.h"
#include "Audio/AudioClipSystem.h"

// ---- UI ----------------------------------------------------------------
#include "UI/UISystem.h"
#include "UI/WorldSpaceUISystem.h"

class Renderer;
struct GameModeContext;
struct FrameContext;

class App
{
public:
    App();
    int Run();

private:
    // ---- Run() helpers ----------------------------------------------------
    void RegisterResourceLoaders(IGraphicsDevice& backend);
    void InitUIFont(IGraphicsDevice& backend);
    void InitLuaBindings(Renderer& renderer);
    void InitAudio();
    void WireEditor(IGraphicsDevice& backend, Renderer& renderer, GameModeContext& ctx);
    void RegisterTickSystems(IGraphicsDevice& backend, Renderer& renderer,
                              GameModeContext& ctx);
    void RefreshMainCamera();
    void TickCamera(float dt, const GameModeContext& ctx);
    void UpdateViewportSize(GameModeContext& ctx, IGraphicsDevice& backend);
    void RunFixedPhysicsLoop(float scaledDt, FrameContext& frameCtx);
    void ShutdownAllSystems(IGraphicsDevice& backend);

    // Window must come first — its dtor (owns the GFX device) runs LAST,
    // after every GPU-resource-owning system has been destroyed.
    Window wnd;

    // ---- Resource systems (GPU resources released in ShutdownAllSystems) --
    Resource::ResourceManager      m_resourceMgr;
    Resource::TextureSystem        m_textureSys;
    Resource::MeshSystem           m_meshSys;
    Resource::MeshLibrary          m_meshLib;
    Resource::MaterialSystem       m_matSys;
    Resource::AssetManager         m_assetMgr;
    Resource::AnimationClipSystem  m_animClipSys;
    Audio::AudioClipSystem         m_audioClipSys;

#ifdef WITH_EDITOR
    EditorLayer  m_editorLayer;
#endif
    GameModeStack m_gameModeStack;

    // ---- Engine state (shared across game modes; survives level switches) ----
    // IGameModes operate on this World rather than owning their own; systems
    // are stateful (Lua VM, Jolt, timers) so per-mode gameplay stacks on top.
    World                       m_world;
    CameraSystem                m_cameraSystem;
    PlayerControllerSystem      m_playerCtrl;
    ScriptSystem                m_scriptSystem;
    AI::AISystem                m_aiSystem;
    AI::AILODSystem             m_aiLODSystem;
    DX12Physics::PhysicsSystem  m_physicsSystem;
    Nav::NavMeshSystem          m_navSystem;
    Audio::AudioEngine          m_audioEngine;
    Audio::AudioSystem          m_audioSystem;
    Audio::Audio3DSystem        m_audio3DSystem;
    Entity                      m_cameraEntity{ NullEntity };

    // ---- UI ---------------------------------------------------------------
    // Screen-space UISystem drives UIRoot widgets/hit-test/DrawList submit.
    // WorldSpaceUISystem feeds WorldUIBillboardPass (separate from UIPass).
    UI::UISystem                m_uiSystem;
    UI::UIInputState            m_uiInput;
    UI::WorldSpaceUISystem      m_worldSpaceUISystem;

    // ---- Phase-based scheduler (DesignMd/System_Scheduler_Architecture.md) -
    // SystemRegistry partitions thin ISystem adapters by TickPhase; Scheduler
    // drives one phase at a time; CommandBuffer flushes at every phase edge.
    SystemRegistry m_systemRegistry;
    Scheduler      m_scheduler;
    CommandBuffer  m_commandBuffer;

    // Externalised physics accumulator (T2). Cleared on world reload so a
    // long load doesn't burn 5 catch-up steps the first tick.
    float        m_physicsAccumulator{ 0.0f };
    bool         m_viewportFullscreen{ false };
};
