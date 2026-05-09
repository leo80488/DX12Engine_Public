#pragma once

#include "System/Window.h"
#ifdef WITH_EDITOR
#include "Editor/EditorLayer.h"
#endif
#include "Scene/SceneManager.h"
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "Resource/MeshSystem.h"
#include "Resource/MeshLibrary.h"
#include "Resource/MaterialSystem.h"
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"
#include "ECS/ECS.h"
#include "ECS/CameraSystem.h"
#include "Scripting/ScriptSystem.h"
#include "AI/AISystem.h"
#include "AI/AILODSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioSystem.h"
#include "Audio/Audio3DSystem.h"
#include "Audio/AudioClipSystem.h"
#include "UI/UISystem.h"
#include "UI/WorldSpaceUISystem.h"

class App
{
public:
    App();

    // Main loop until WM_QUIT is received.
    int Run();

private:
    // Window must be declared first so its destructor (which owns the GFX device)
    // runs LAST — after all GPU-resource-owning systems are already destroyed.
    Window       wnd;

    // Resource systems — GPU resources must be explicitly released via Shutdown()
    // inside Run() before the loop exits, while the GFX device is still alive.
    Resource::ResourceManager  m_resourceMgr;
    Resource::TextureSystem    m_textureSys;
    Resource::MeshSystem       m_meshSys;
    Resource::MeshLibrary      m_meshLib;       // new first-class mesh-pool store (P1-P6)
    Resource::MaterialSystem   m_matSys;
    Resource::AssetManager         m_assetMgr;      // path-dedup cache over meshSys + textureSys
    Resource::AnimationClipSystem  m_animClipSys;   // path-dedup cache for .ianim clip resources
    Audio::AudioClipSystem         m_audioClipSys;  // path-dedup cache for .aclip audio resources

#ifdef WITH_EDITOR
    EditorLayer  m_editorLayer;
#endif
    SceneManager m_sceneManager;

    // ---- Engine state (shared across IScenes; survives level switches) ------
    // World is the single ECS data container — IScenes operate on this rather
    // than owning their own. Systems are stateful (Lua VM, Jolt, timers) and
    // also live here so per-scene gameplay can stack on top without rebuild.
    World                       m_world;
    CameraSystem                m_cameraSystem;
    ScriptSystem                m_scriptSystem;
    AI::AISystem                m_aiSystem;
    AI::AILODSystem             m_aiLODSystem;
    DX12Physics::PhysicsSystem  m_physicsSystem;
    Audio::AudioEngine          m_audioEngine;
    Audio::AudioSystem          m_audioSystem;
    Audio::Audio3DSystem        m_audio3DSystem;
    Entity                      m_cameraEntity{ NullEntity };

    // UI subsystem — drives UIRoot widgets, hit-testing, and DrawList submit.
    UI::UISystem                m_uiSystem;
    UI::UIInputState            m_uiInput;
    // World-space UI: per-frame fade/scale/cull + DamageNumber lifetimes.
    // Render path is WorldUIBillboardPass (owned by Renderer, separate from UIPass).
    UI::WorldSpaceUISystem      m_worldSpaceUISystem;

    bool         m_viewportFullscreen{ false };
};
