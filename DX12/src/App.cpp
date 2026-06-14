#include "App.h"

// ---- Engine / ECS ----------------------------------------------------------
#include "ECS/EngineSystems.h"
#include "ECS/TimelineSystem.h"
#include "ECS/NotifyConsumerSystems.h"
#include "ECS/VideoSystem.h"
#include "ECS/LifetimeComponent.h"
#include "ECS/FrameContext.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform / GlobalTransform
#include "ECS/LuaCharacterStateBindings.h"
#include "ECS/LuaPlayerBindings.h"
#include "ECS/CameraStackSystem.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/GuidRegistry.h"

// ---- Scene -----------------------------------------------------------------
#include "Scene/TestScene.h"
#include "Scene/DataScene.h"
#include "Scene/LuaSceneBindings.h"
#include "Scene/ShaderLabScene.h"
#include "Scene/MeshSpawner.h"
#include "Scene/TransformSystem.h"

// ---- Graphics --------------------------------------------------------------
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/Renderer.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "Tools/CollisionMeshBaker.h"

// ---- Resource (loaders + importers) ----------------------------------------
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "Resource/MeshSystem.h"
#include "Resource/MaterialSystem.h"
#include "Resource/BCCompressor.h"
#include "Resource/AssetFS.h"
#include "Resource/TextureLoader.h"
#include "Resource/ShaderLoader.h"
#include "Resource/MaterialLoader.h"
#include "Resource/AnimationLoader.h"
#include "Resource/AnimationClipSystem.h"
#include "Audio/AudioClipLoader.h"
#ifdef WITH_EDITOR
// Importers are editor-only; runtime loads pre-imported .itex/.imsh/.imat/.ianim/.aclip.
#include "Resource/TextureImporter.h"
#include "Resource/ShaderImporter.h"
#include "Resource/MaterialImporter.h"
#include "Resource/AnimationImporter.h"
#include "Resource/VmdImporter.h"
#include "Audio/AudioImporter.h"
#endif

// ---- Audio / UI / AI / Nav / Physics --------------------------------------
#include "Audio/AudioEvents.h"
#include "UI/UISystem.h"
#include "UI/UIComponents.h"
#include "UI/UICanvas.h"
#include "UI/Font.h"
#include "UI/LuaUIBindings.h"
#include "AI/LuaBTBindings.h"
#include "Nav/NavAgentSystem.h"
#include "Nav/LuaNavBindings.h"
#include "Intent/LuaIntentBindings.h"
#include "Intent/LuaAIBindings.h"
#include "Physics/LuaPhysicsBindings.h"
#include "Resource/PrefabSerializer.h"   // Resource::LoadPrefab (Engine.SpawnPrefab)
#include "Scripting/LuaMathTypes.h"      // LuaVec3 (camera getters)
#include "PostProcess/LuaPostProcessBindings.h"
#include "ECS/PostProcessResolveSystem.h"

// ---- System / Input --------------------------------------------------------
#include "System/TaskSystem.h"
#include "System/Log.h"
#include "System/Timer.h"
#include "System/FileWatcher.h"
#include "System/EventBus.h"
#include "System/Mouse.h"
#include "System/Keyboard.h"
#include "Input/InputSystem.h"

#ifdef WITH_EDITOR
#include "Editor/EditorLayer.h"
#include "imgui/imgui.h"
#endif

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static const UINT  WindowWidth  = 1920;
static const UINT  WindowHeight = 1080;
static const float kClearColor[4] = { 0.05f, 0.05f, 0.1f, 1.0f };

// ===========================================================================
// Construction / Run
// ===========================================================================

App::App()
    : wnd(WindowWidth, WindowHeight, L"DX12 — Deferred Rendering")
{
    LOG_INFO("App initialized");
#ifdef WITH_EDITOR
    m_editorLayer.OnAttach();
#else
    m_viewportFullscreen = true;   // Game build: scene always fills backbuffer.
#endif
}

int App::Run()
{
    // Release ships with game.ipak (single sealed archive). Editor/dev runs
    // fall back to loose files automatically via AssetFS::ReadFile.
    ::Resource::AssetFS::Get().Mount("game.ipak");

    IGraphicsDevice& backend = wnd.Gfx();

    RegisterResourceLoaders(backend);
    backend.InitPSOLibrary("pso_cache.bin");

    m_assetMgr.Init(m_meshSys, m_textureSys, m_resourceMgr, backend);
    m_animClipSys.Init(m_resourceMgr);

    // GUID registry — install destroy listener so entities with
    // GuidComponent auto-unregister when they die. SceneSerializer
    // separately Clears + RebuildFromWorld around each Load Scene.
    ECS::GuidRegistry::Get().RegisterWithWorld(m_world);

    InitUIFont(backend);

#ifndef WITH_EDITOR
    // Game build: UI alone owns mouse/keyboard capture (no ImGui involved).
    Window::SetWantCaptureMouse(&UI::UISystem::GlobalWantsCaptureMouse);
    Window::SetWantCaptureKeyboard(&UI::UISystem::GlobalWantsCaptureKeyboard);
#endif

    Renderer renderer(backend);
    renderer.Compile();
    renderer.SetMaterialSystem(&m_matSys);
    renderer.SetMeshSystem(m_assetMgr.GetMeshSystem());
    renderer.SetMeshLibrary(&m_meshLib);
    renderer.SetTextureSystem(m_assetMgr.GetTextureSystem());
    renderer.SetResourceManager(m_assetMgr.GetResourceManager());

    // ---- Engine systems (App-owned; persist across scene switches) --------
    m_scriptSystem.Initialize();
    m_scriptSystem.BindWorld(m_world);
    // Boot scan loads services/*.lua and systems/*.lua; Logic templates load
    // lazily on first ScriptComponent sight (per Script_Architecture §6.2).
    m_scriptSystem.ScanScriptDirectory("asset/scripts");

    InitLuaBindings(renderer);

    m_physicsSystem.Init();
    InitAudio();

    // ---- Build GameModeContext -------------------------------------------
    GameModeContext ctx{ backend, renderer };
    ctx.resourceMgr = &m_resourceMgr;
    ctx.textureSys  = &m_textureSys;
    ctx.meshSys     = &m_meshSys;
    ctx.meshLib     = &m_meshLib;
    ctx.matSys      = &m_matSys;
    ctx.assetMgr    = &m_assetMgr;
    ctx.world       = &m_world;
    ctx.navSys      = &m_navSystem;
    ctx.sceneManager = &m_sceneManager;

    std::unique_ptr<IGameMode> pendingMode;
    ctx.requestReplaceMode = [&pendingMode](std::unique_ptr<IGameMode> next) {
        pendingMode = std::move(next);
    };
    // Faded variant: scenes call this for player-facing switches; the manager
    // drives fade-out/loading/fade-in and triggers the raw swap above when the
    // screen is fully black (see SceneTransitionManager::Tick).
    ctx.beginTransition = [this](std::unique_ptr<IGameMode> next) {
        m_transition.Begin(std::move(next));
    };

    // ---- Data-driven scene system ----------------------------------------
    // SceneManager owns the scene registry (game.json) + the unified, blocking
    // world-reload path. It routes Scene.Load() through the SAME transition
    // closures the game modes use, wrapping a new DataScene in the fade.
    m_sceneManager.Configure(&backend, &renderer, &m_world, &m_assetMgr,
                             &m_animClipSys, &m_navSystem, &m_scriptSystem,
                             &m_physicsSystem);
    m_sceneManager.SetTransitionHooks(ctx.beginTransition, ctx.requestReplaceMode);
    m_sceneManager.LoadManifest("game.json");

    // Initial mode varies by build: ShaderLab tool / Editor target / Game flow.
    // Editor/ShaderLab keep their dedicated code-driven tool modes; the Game
    // build boots the data-driven startup scene declared in game.json via the
    // single generic DataScene (no more hardcoded TitleScene/GameScene/EndScene).
#ifdef WITH_SHADERLAB
    m_gameModeStack.PushMode(std::make_unique<ShaderLabScene>(), ctx);
#elif defined(WITH_EDITOR)
    m_gameModeStack.PushMode(std::make_unique<TestScene>(), ctx);
#else
    {
        const std::string startup = m_sceneManager.StartupScene();
        if (startup.empty())
            LOG_WARNING("App: game.json has no startup_scene — DataScene will fall "
                        "back to a default world");
        m_gameModeStack.PushMode(std::make_unique<DataScene>(startup), ctx);
    }
#endif

    // Renderer-side system wiring needed by BOTH Game and Editor builds. These
    // used to live in WireEditor (editor-only), which left them null in a Game
    // build: CharacterStateSystem then had no AnimationClipSystem, so Lua
    // state-machine clips (Character.AddState/SetState) never loaded → skinned
    // characters stayed in bind pose ("no animation"); foot-IK also lost its
    // physics ground-raycast source. Must run after the renderer's skin
    // subsystem exists (it is, by construction) and after m_animClipSys.Init().
    renderer.SetPhysicsSystem(&m_physicsSystem);
    renderer.SetAnimationClipSystem(&m_animClipSys);

#ifdef WITH_EDITOR
    WireEditor(backend, renderer, ctx);
#endif

    RegisterTickSystems(backend, renderer, ctx);

    Timer timer;

    // Watch shaders/ for .hlsl/.hlsli writes; on change, drop in-memory shader
    // + PSO caches so every reload-aware pass re-fetches. Dev-only but cheap.
    HotReload::FileWatcher shaderWatcher;
    shaderWatcher.Init("shaders");

    while (true)
    {
        // Hot-reload — drained BEFORE BeginFrame so no in-flight CL still
        // references the old PSOs. FlushAndWait is the easy hammer for v1.
        {
            const auto changed = shaderWatcher.PollChanges();
            if (!changed.empty())
            {
                LOG_INFO("Hot-reload: %zu shader file(s) changed", changed.size());
                backend.FlushAndWait();
                renderer.ReloadShaders();
            }
        }

        if (auto ecode = Window::ProcessMessage())
        {
            m_gameModeStack.PopMode();
            ShutdownAllSystems(backend);
            backend.SavePSOLibrary("pso_cache.bin");
            return *ecode;
        }

        const float dt = timer.Mark();

        // Snapshot input ONCE per frame so every consumer below sees the same
        // edge-state (WasKeyPressed/Released). Must precede every input read.
        Input::Get().Update();

        LARGE_INTEGER cpuStart, cpuEnd, cpuFreq;
        QueryPerformanceCounter(&cpuStart);

#ifdef WITH_EDITOR
        if (Input::Get().WasKeyPressed(VK_F11))
            m_viewportFullscreen = !m_viewportFullscreen;
#endif

        UpdateViewportSize(ctx, backend);

#ifdef WITH_EDITOR
        ctx.viewportRightMouseHeld = m_editorLayer.IsViewportRightDragging();
        m_editorLayer.GetViewportMouseDelta(ctx.mouseViewportDX, ctx.mouseViewportDY);
#else
        // Game build (no editor viewport): drive the free-look camera straight
        // from the OS mouse. Right button held = look; the delta is this frame's
        // mouse movement in pixels, matching the editor's ImGui MouseDelta
        // convention so CameraSystem sensitivity feels identical. Without this
        // the camera never sees mouse input (the editor used to be the only
        // source of mouseViewportDX/DY), so mouse-look appeared dead in Game.
        {
            Mouse&     ms  = Mouse::GetInstance();
            const bool rmb = ms.RightIsPressed();
            const auto mp  = ms.GetPos();
            if (rmb && m_prevMouseValid)
            {
                ctx.mouseViewportDX = static_cast<float>(mp.first  - m_prevMouseX);
                ctx.mouseViewportDY = static_cast<float>(mp.second - m_prevMouseY);
            }
            else
            {
                ctx.mouseViewportDX = 0.f;
                ctx.mouseViewportDY = 0.f;
            }
            ctx.viewportRightMouseHeld = rmb;
            m_prevMouseX     = mp.first;
            m_prevMouseY     = mp.second;
            m_prevMouseValid = true;
        }
#endif

        // Promote async-loaded resources to GPU (2 ms budget) + flush dirty mats.
        m_resourceMgr.ProcessPendingGPUUploads(2.0f);
        m_matSys.Tick(m_textureSys, backend);

        ctx.deltaTime = dt;

        RefreshMainCamera();
        TickCamera(dt, ctx);

        // ---- Editor play/stop/step gate -----------------------------------
        // Editor: Playing → real dt. Stopped/Paused → skip ticks unless Step
        // was clicked (one fixed 1/60s frame). Game build: always real dt.
        float effectiveDt = dt;
        bool  runUpdate   = true;
#ifdef WITH_EDITOR
        {
            const ViewportPlayState ps = m_editorLayer.GetPlayState();
            const bool wantsStep       = m_editorLayer.ConsumeWantsStepFrame();
            const bool isPaused        = (ps == ViewportPlayState::Stopped ||
                                          ps == ViewportPlayState::Paused);
            if (isPaused)
            {
                if (wantsStep) effectiveDt = 1.0f / 60.0f;
#ifndef WITH_SHADERLAB
                // ShaderLab ignores Stopped/Paused — tools-driven animation
                // (turntable, future timeline scrub) needs to keep ticking.
                else           runUpdate   = false;
#endif
            }
        }
#endif
        // scaledDt feeds gameplay/physics/AI; deltaTime feeds hit-stop / UI.
        const float scaledDt = effectiveDt * m_scriptSystem.GetTimeScale();

        FrameContext frameCtx;
        frameCtx.deltaTime        = effectiveDt;
        frameCtx.scaledDeltaTime  = scaledDt;
        frameCtx.fixedDeltaTime   = 1.0f / 60.0f;
        frameCtx.frameIndex       = ctx.frame;
        frameCtx.isFixedTickPhase = false;
        frameCtx.runUpdate        = runUpdate;
        frameCtx.cameraEntity     = static_cast<uint32_t>(m_cameraEntity);
        frameCtx.commandBuffer    = &m_commandBuffer;
        frameCtx.jobSystem        = &TaskSystem::Get();

        // ---- Variable-rate simulation phases ------------------------------
        m_scheduler.RunPhase(TickPhase::Input,             m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::GameplayPreLogic,  m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::GameplayLogic,     m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::GameplayPostLogic, m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::AI,                m_world, frameCtx);

        RunFixedPhysicsLoop(scaledDt, frameCtx);

        // Advance any in-flight scene transition (fade/loading). When fade-out
        // completes it calls ctx.requestReplaceMode, which the drain below picks
        // up the SAME frame, so the blocking scene load runs behind full black.
        if (runUpdate)
            m_transition.Tick(effectiveDt, ctx.requestReplaceMode);

        // Mode transition drain — kept inline (not a System adapter) because
        // IGameMode::Init needs the live GameModeContext built from local refs.
        if (runUpdate && pendingMode)
        {
            m_gameModeStack.PopMode();
            m_gameModeStack.PushMode(std::move(pendingMode), ctx);
            pendingMode.reset();
        }

        m_scheduler.RunPhase(TickPhase::PhysicsInterpolation, m_world, frameCtx);

        // Drain the upcoming swap-chain slot's GPU fence BEFORE Animation —
        // Animation writes per-frame upload-buffer slots the GPU is still
        // reading from N-FrameCount ago. backend.BeginFrame sees the flag and
        // skips its own redundant wait.
        backend.WaitForNextFrameSlot();
        m_scheduler.RunPhase(TickPhase::Animation,        m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::BoneAttachment,   m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::SecondaryPhysics, m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::PreRender,        m_world, frameCtx);

        // Forward the resolved Live camera to the Renderer. CameraResolveSystem
        // (PreRender) just blended the stack into LiveCameraComponent on the
        // Main channel entity. Fall back to the legacy CameraComponent path
        // if the channel entity doesn't exist yet (first frame before TickCamera
        // backfilled it, or scenes without any camera).
        bool sentCamera = false;
        if (Entity ch = Camera::FindChannelEntity(m_world, Camera::kMainChannel);
            ch != NullEntity)
        {
            if (auto* live = m_world.GetComponent<LiveCameraComponent>(ch))
            {
                RenderCamera rc;
                rc.position = live->position;
                rc.forward  = live->forward;
                rc.fov      = live->fov;
                rc.nearZ    = live->nearZ;
                rc.farZ     = live->farZ;
                rc.historyValid = live->historyValid;
                renderer.SetCamera(rc);
                sentCamera = true;
            }
        }
        if (!sentCamera)
        if (CameraComponent* cam = m_world.GetComponent<CameraComponent>(m_cameraEntity))
        {
            if (const GlobalTransform* gt =
                    m_world.GetComponent<GlobalTransform>(m_cameraEntity))
            {
                const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&gt->matrix);
                RenderCamera rc;
                DirectX::XMStoreFloat3(&rc.position, world.r[3]);
                DirectX::XMStoreFloat3(&rc.forward,
                                       DirectX::XMVector3Normalize(world.r[2]));
                rc.fov   = cam->fov;
                rc.nearZ = cam->nearZ;
                rc.farZ  = cam->farZ;
                renderer.SetCamera(rc);
            }
        }

        m_scheduler.RunPhase(TickPhase::Render,     m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::PostRender, m_world, frameCtx);

        QueryPerformanceCounter(&cpuEnd);
        QueryPerformanceFrequency(&cpuFreq);
#ifdef WITH_EDITOR
        m_editorLayer.SetCPUFrameTime(
            static_cast<float>(cpuEnd.QuadPart - cpuStart.QuadPart) * 1000.0f
            / static_cast<float>(cpuFreq.QuadPart));

        Entity pickedEntity;
        if (renderer.ResolvePick(pickedEntity))
            m_editorLayer.SetPickResult(pickedEntity);
#else
        (void)cpuEnd; (void)cpuFreq;
#endif

        ++ctx.frame;
    }
}

// ===========================================================================
// Run() helpers — initialisation
// ===========================================================================

void App::RegisterResourceLoaders(IGraphicsDevice& backend)
{
    // Internal format → Resource object.
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::TextureLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::ShaderLoader>(&backend));
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::MaterialLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::AnimationLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Audio::AudioClipLoader>());

#ifdef WITH_EDITOR
    // External source → internal blob.
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::TextureImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::ShaderImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::MaterialImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::AnimationImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::VmdImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Audio::AudioImporter>());
#endif
}

void App::InitUIFont(IGraphicsDevice& backend)
{
    // FGMiraiRen ships with the engine. Falls back silently to a no-op text
    // path if the TTF isn't on disk.
    constexpr const char* kDefaultTTF = "asset/font/FGMiraiRen.ttf";
    if (UI::DefaultFont().Init(backend, kDefaultTTF, /*pixelSize*/ 24.f))
        UI::DefaultFont().InstallAsGlobal();
    else
        LOG_WARNING("UI font init failed — text rendering disabled");
}

void App::InitLuaBindings(Renderer& renderer)
{
    sol::state* lua = m_scriptSystem.GetLua();
    if (!lua) return;

    AI::RegisterLuaBTBindings(*lua);
    UI::RegisterLuaUIBindings(*lua, m_world);
    Nav::RegisterLuaNavBindings(*lua, m_navSystem, m_world);
    Intent::RegisterLuaIntentBindings(*lua, m_world);
    Intent::RegisterLuaAIBindings(*lua, m_world);
    DX12Physics::RegisterLuaPhysicsBindings(*lua, m_physicsSystem, m_world);
    RegisterLuaCharacterStateBindings(*lua, m_world);
    RegisterLuaPlayerBindings(*lua, m_world);
    PostProcess::RegisterLuaPostProcessBindings(*lua, m_world);
    Scene::RegisterLuaSceneBindings(*lua, m_sceneManager);
    m_aiSystem.Init(lua);

    // VFX.SpawnAfterimage(entity, lifetime, r, g, b) — pushes a skinned-pose
    // ghost-trail snapshot into the renderer's afterimage pool (HDR colour).
    {
        sol::table vfx = lua->create_named_table("VFX");
        vfx.set_function("SpawnAfterimage",
            [&renderer](uint32_t entity, float lifetime, float r, float g, float b)
            {
                if (auto* sys = renderer.GetAfterimageSystem())
                    sys->Spawn(static_cast<Entity>(entity), lifetime,
                               DirectX::XMFLOAT4{ r, g, b, 1.0f });
            });
    }

    // Engine.SpawnPrefab + active-camera world pose. These live HERE (not in
    // ScriptSystem) because only App::InitLuaBindings holds the Renderer&; they
    // are appended to the Engine table that ScriptSystem::RegisterBindings has
    // already created (Initialize() runs before InitLuaBindings).
    {
        sol::table engineTbl = (*lua)["Engine"];

        // Engine.SpawnPrefab(path, x,y,z) -> entityId. Instantiates a .ipfb
        // tree (root + descendants) then re-positions the root's LocalTransform.
        engineTbl.set_function("SpawnPrefab",
            [this, &renderer](const std::string& path,
                              float x, float y, float z) -> uint32_t
            {
                Entity root = Resource::LoadPrefab(
                    path, m_world, m_assetMgr, &renderer, &m_animClipSys);
                if (root == NullEntity) return static_cast<uint32_t>(NullEntity);
                if (auto* lt = m_world.GetComponent<LocalTransform>(root))
                    lt->translation = { x, y, z };
                return static_cast<uint32_t>(root);
            });

        // Active-camera world pose (mirrors RenderView in Renderer::GetView()).
        engineTbl.set_function("GetCameraPosition",
            [&renderer]() -> LuaVec3 {
                const RenderView& v = renderer.GetView();
                return LuaVec3{ v.cameraPosition.x, v.cameraPosition.y, v.cameraPosition.z };
            });
        engineTbl.set_function("GetCameraForward",
            [&renderer]() -> LuaVec3 {
                const RenderView& v = renderer.GetView();
                return LuaVec3{ v.cameraForward.x, v.cameraForward.y, v.cameraForward.z };
            });
    }

    // BT Actions / Conditions — must run AFTER RegisterLuaBTBindings and
    // RegisterLuaNavBindings so the script can append to those tables.
    // Order matters: base actions.lua first, then any extension scripts
    // that depend on Actions / Conditions / BT / Character / Nav already
    // being populated.
    constexpr const char* kBTScripts[] = {
        "asset/ai/actions.lua",
        "asset/ai/enemy_actions.lua",
    };
    for (const char* path : kBTScripts)
    {
        // Read via AssetFS (pak first, disk fallback) so these load from
        // game.ipak in a packed Game build instead of only off loose disk.
        std::string src;
        if (!::Resource::AssetFS::Get().ReadFileText(path, src))
        {
            LOG_ERROR("App: failed to read '%s' (not in pak or on disk)", path);
            continue;
        }
        auto r = lua->safe_script(src, sol::script_pass_on_error,
                                  std::string("@") + path);
        if (!r.valid())
        {
            sol::error err = r;
            LOG_ERROR("App: failed to load '%s': %s", path, err.what());
        }
        else
        {
            LOG_INFO("App: loaded BT actions from '%s'", path);
        }
    }
}

void App::InitAudio()
{
    // AudioSystem installs an entity-destroy listener so component teardown
    // stops live voices and releases clip refcounts.
    m_audioClipSys.Init(m_resourceMgr);
    if (m_audioEngine.Initialize())
    {
        m_audioSystem.BindEngine(m_audioEngine);
        m_audioSystem.BindClipSystem(m_audioClipSys);
        m_audioSystem.BindWorld(m_world);
        m_audio3DSystem.BindEngine(m_audioEngine);
    }
}

#ifdef WITH_EDITOR
void App::WireEditor(IGraphicsDevice& backend, Renderer& renderer, GameModeContext& ctx)
{
    m_editorLayer.SetGraphicsDevice(&backend);
    m_editorLayer.SetResourceSystems(&m_textureSys, &m_resourceMgr);
    m_editorLayer.SetAssetManager(&m_assetMgr);
    m_editorLayer.SetRenderer(&renderer);
    m_editorLayer.SetAnimationClipSystem(&m_animClipSys);
    m_editorLayer.SetAnimationSystem(renderer.GetAnimationSystem());
    m_editorLayer.SetPhysicsSystem(&m_physicsSystem);
    // NOTE: renderer.SetPhysicsSystem / SetAnimationClipSystem moved to App::Run
    // (before this WireEditor call) so Game builds get them too — they are not
    // editor-specific. FootIK raycasts via physics; CharacterStateSystem
    // lazy-acquires state clips through the AnimationClipSystem.
    m_editorLayer.SetNavMeshSystem(&m_navSystem);
    m_editorLayer.SetAISystem(&m_aiSystem);
    m_editorLayer.SetScriptSystem(&m_scriptSystem);   // exposed-var inspector
    m_editorLayer.SetDebugDrawSystem(&m_debugDraw);   // debug category toggles
    m_editorLayer.RegisterDefaultEditors();   // must be after SetRenderer()
    m_editorLayer.SetAssetDirectory("asset/");
    m_editorLayer.SetGPUProfiler(&static_cast<GraphicsDX12&>(backend).GetGPUProfiler());
    m_editorLayer.InitImGuiBackends();        // must be after SetGraphicsDevice

    // Combine ImGui's capture predicate with the game-layer UI so Window
    // suppresses input when either layer wants the cursor/keys.
    Window::SetWantCaptureMouse([]() -> bool {
        return ImGui::GetIO().WantCaptureMouse
            || UI::UISystem::GlobalWantsCaptureMouse();
    });
    Window::SetWantCaptureKeyboard([]() -> bool {
        return ImGui::GetIO().WantCaptureKeyboard
            || UI::UISystem::GlobalWantsCaptureKeyboard();
    });

    m_editorLayer.SetCreateMeshCallback([&ctx](int meshType) {
        if (!ctx.world) return;
        MeshSpawner::Spawn(meshType, *ctx.world);
    });

    m_editorLayer.SetPickCallback([&renderer, &ctx](float px, float py) {
        if (ctx.viewportW == 0 || ctx.viewportH == 0) return;
        renderer.RequestPick(static_cast<int>(px), static_cast<int>(py));
    });
}
#else
void App::WireEditor(IGraphicsDevice&, Renderer&, GameModeContext&) {}
#endif

void App::RegisterTickSystems(IGraphicsDevice& backend, Renderer& renderer,
                               GameModeContext& ctx)
{
    // Single source of tick order for the ECS scheduler (see
    // DesignMd/System_Scheduler_Architecture.md). Order INSIDE a phase is
    // registration order; phases run sequentially per kPhaseDescriptors.
    auto& reg = m_systemRegistry;

    // GameplayPreLogic — LifetimeSystem first so expired VFX die before
    // scripts / AI iterate. Notify-spawned emitters from the previous
    // frame's TimelineSystem all expire here.
    reg.Add<LifetimeSystem>();
    reg.Add<ScriptTimerSystem>(m_scriptSystem);

    // Video decode — submits on the dedicated D3D12 video queue; runs early
    // so the GPU can overlap decode with the graphics pipeline. Renderer
    // calls IVideoDecoderBackend::AddDecodeDependency before sampling the
    // NV12 output, so by the time the PS reads it the frame is retired.
    reg.Add<VideoSystem>(backend);

    // GameplayLogic
    reg.Add<ScriptLogicSystem>(m_scriptSystem);

    // AI — LOD → BT → AITactical → NavAgent → Player. Layered movement
    // model (DesignMd/character_movement_architecture.md §3.1):
    //   * BT writes AIIntentComponent (strategic goal) and may also
    //     directly write NavAgentComponent fields via `Intent.MoveTo`.
    //   * AITacticalSystem translates AIIntent → NavAgent.destination +
    //     facingMode + facingTarget. Skipped for goal=Idle so direct
    //     Intent.MoveTo writes survive.
    //   * NavAgentSystem reads NavAgent.destination, queries NavMesh for
    //     a path, writes CharacterController.desiredHorizontalVelocity
    //     and LocalTransform.rotation.
    //   * PlayerController writes the same desiredHorizontalVelocity for
    //     the player (camera-relative WASD).
    //   * KCC step inside PhysicsStepSystem (next phase) does the
    //     actual sweep-and-slide via Jolt CharacterVirtual.
    reg.Add<AILODTickSystem>(m_aiLODSystem);
    reg.Add<AIBTTickSystem>(m_aiSystem);
    reg.Add<AITacticalTickSystem>();
    reg.Add<NavAgentTickSystem>(m_navSystem);
    reg.Add<PlayerControlTickSystem>(m_playerCtrl);

    // GameplayPostLogic — GameModeStack only (pending mode drained inline).
    reg.Add<GameModeStackTickSystem>(m_gameModeStack);

    // FixedPhysics — single adapter; PhysicsSystem owns its 60 Hz accumulator.
    reg.Add<PhysicsStepSystem>(m_physicsSystem);

    // PhysicsInterpolation — Propagate (always-on so gizmo drags fan out
    // even when paused) → PhysicsInterp apply → CameraFollow resolve.
    reg.Add<TransformPropagateSystem>();
    reg.Add<PhysicsInterpApplySystem>(m_physicsSystem);
    reg.Add<CameraFollowResolveSystem>(m_physicsSystem);
    // Camera-stack behavior systems — order is Follow then Aim, matching
    // the design doc rationale (Follow is the baseline; Aim layers on as a
    // higher-priority VCam during combat/aim states). Both write
    // CameraPoseComponent on their VCam entities; CameraStack / CameraResolve
    // (PreRender phase) read these.
    reg.Add<FollowCameraTickSystem>();
    reg.Add<AimCameraTickSystem>(&m_physicsSystem);

    // PreRender — Audio sits here so collision SFX fire same frame as the
    // collision (audio runs after PhysicsInterpolation, before Render).
    reg.Add<AudioTickSystem>(m_audioClipSys, m_audioSystem,
                              m_audio3DSystem, m_audioEngine);

    // Animation — wraps the chain (character state → AnimSystem sample → IK
    // → ChainPhys → L2W → Socket/Follow → bone AABB merge → SkinMatrix →
    // BuildSkinJobs). Must run BEFORE Render so SkinningPass sees the offsets.
    reg.Add<RendererAnimationChainSystem>(renderer);

    // TimelineSystem must run AFTER the animation chain so the prev→curr
    // time window tests against the latest sampled primaryTime. It reads
    // clip-authored notify tracks via the ClipLibrary (Path A) plus any
    // per-entity TimelineComponent overrides (Path B).
    reg.Add<TimelineSystem>(renderer.GetClipLibrary());

    // BoneAttachment — Notify consumers drain the mailboxes TimelineSystem
    // wrote in the previous phase, same render frame. Order is arbitrary
    // (distinct mailbox types) but fixed for stable profiler output.
    reg.Add<HitboxSystem>();
    reg.Add<VFXSpawnSystem>();
    reg.Add<CameraEffectSystem>();
    reg.Add<AudioPlaySystem>();
    reg.Add<StateToggleSystem>();
    reg.Add<GenericNotifyDispatcher>();

    // Camera stack pipeline — must run in PreRender, AFTER all behavior
    // systems (PhysicsInterpolation) finished writing CameraPoseComponent
    // and BEFORE Render. Stack tick advances blend state machine; Resolve
    // produces LiveCameraComponent the Renderer reads; Shake layers
    // additive trauma noise on top.
    reg.Add<CameraStackTickSystem>();
    reg.Add<CameraResolveTickSystem>();
    reg.Add<CameraShakeTickSystem>();
    // After the camera blend resolves: turn volumes + override stack into the
    // frame's ResolvedPostProcessSettings (PostProcess::Runtime).
    reg.Add<PostProcessResolveSystem>();

    // Render — the GPU recording block (backend.BeginFrame → Renderer →
    // backend.EndFrame, with UI pump + EditorLayer ImGui chrome) runs as a
    // lambda because it captures ~15 outer refs; wrapping in a services
    // struct would be more boilerplate than abstraction value.
    reg.Add<RenderSystem>("RenderSystem",
        [&, this](World& world, const FrameContext& frameCtx)
    {
        // BeginFrame does a pipelined wait + drains this backbuffer's
        // deferred-release slot. Any Destroy/Release after this point queues
        // for a future BeginFrame and is safe regardless of in-flight GPU.
        RHI::CommandList primaryCL = backend.BeginFrame();
        backend.SetViewportSize(ctx.viewportW, ctx.viewportH);

#ifdef WITH_EDITOR
        // Drain queued editor actions (Load World, ...) after BeginFrame
        // sync, before any new GPU recording. Inline-in-handler tears down
        // descriptor slots that earlier ImGui::Image calls captured → UAF.
        m_editorLayer.ProcessPendingActions();
#endif

        // TextureSystem::Tick MUST come after BeginFrame: flushes pending
        // destroys (previous frame is GPU-complete) + promotes RM-ready
        // textures to GPU.
        m_textureSys.Tick(m_resourceMgr, backend);
        m_animClipSys.Tick();
        m_animClipSys.ResolvePendingBinds(world, renderer);

        // UI tick runs AFTER renderer.BeginFrame so world-space anchors
        // project with THIS frame's view-projection. Mouse arrives in
        // window-space; editor builds offset by the viewport panel min.
        auto pumpUIInput = [&]()
        {
            Mouse& ms = Mouse::GetInstance();
            const auto mp = ms.GetPos();
            float vpMinX = 0.f, vpMinY = 0.f;
#ifdef WITH_EDITOR
            if (!m_viewportFullscreen)
                m_editorLayer.GetViewportMin(vpMinX, vpMinY);
#endif
            m_uiInput.mousePos    = { static_cast<float>(mp.first)  - vpMinX,
                                      static_cast<float>(mp.second) - vpMinY };
            m_uiInput.mouseLeft   = ms.LeftIsPressed();
            m_uiInput.mouseRight  = ms.RightIsPressed();
            m_uiInput.mouseMiddle = false;   // engine Mouse has no middle bit yet

            // Skip text/key drain when ImGui has the keyboard — UI is one
            // layer above the world, one below the editor chrome.
            Keyboard& kb = Keyboard::GetInstance();
            const bool imguiKB =
#ifdef WITH_EDITOR
                ImGui::GetIO().WantCaptureKeyboard;
#else
                false;
#endif
            if (!imguiKB)
            {
                while (!kb.CharIsempty())
                    m_uiInput.charsThisFrame.push_back(kb.ReadChar());
                while (auto evt = kb.Readkey())
                {
                    UI::UIInputState::KeyEvt k;
                    k.code = evt->GetCode();
                    k.down = evt->IsPress();
                    m_uiInput.keysThisFrame.push_back(k);
                }
            }
            const Input& kbIn = Input::Get();
            m_uiInput.shift = kbIn.IsKeyDown(VK_SHIFT);
            m_uiInput.ctrl  = kbIn.IsKeyDown(VK_CONTROL);
            m_uiInput.alt   = kbIn.IsKeyDown(VK_MENU);

            UI::UIScreen canvas;
            canvas.size = { static_cast<float>(ctx.viewportW),
                            static_cast<float>(ctx.viewportH) };

            // Re-resolve flat UI-image SRV handles from their stable texture
            // path EVERY frame. A raw cached GPU descriptor handle silently
            // aliases whatever texture now occupies its (recycled) heap slot —
            // e.g. after a font re-bake the slot is reused by the glyph atlas,
            // making the image sample the font. Resolving from the live
            // TextureHandle each frame keeps the bound descriptor in lockstep
            // with the texture's current slot. Code-driven (path-less) images
            // are left to manage srvGpuHandle themselves.
            // Advance sprite-sheet animations BEFORE resolving handles / ticking
            // UI, so the current frame's uv0/uv1 are already set this frame.
            UI::AdvanceSpriteAnimations(world, frameCtx.deltaTime);

            auto resolveUiImageHandle = [&](const std::string& path,
                                            Resource::TextureHandle& handle,
                                            uint64_t& srv)
            {
                if (path.empty()) return;
                if (!handle.IsValid())
                    handle = m_textureSys.Acquire(path, m_resourceMgr, backend);
                if (m_textureSys.IsReady(handle))
                {
                    if (const RHI::Texture* tex = m_textureSys.GetTexture(handle))
                        srv = backend.GetTextureSRVGpuHandle(*tex);
                }
                else
                {
                    srv = 0; // not resident yet → renderer skips it
                }
            };

            if (auto* imgPool = world.GetPool<UI::UIImageComponent>())
                for (auto& img : imgPool->Data())
                    resolveUiImageHandle(img.texturePath, img.texture, img.srvGpuHandle);

            // Canvas UI images use the same stable-path → live-SRV re-resolve.
            // CRUCIAL: a path-less canvas image is a solid-colour panel — force
            // its handle to 0 every frame so a stale value left over from a
            // previously-assigned (then cleared) texture can never survive to
            // alias a recycled font-atlas slot (the "texture polluted by font
            // atlas" bug). Solid panels then bind UIPass's 1x1 white texture.
            if (auto* canvasImgPool = world.GetPool<UI::UIImage>())
                for (auto& img : canvasImgPool->Data())
                {
                    if (img.texturePath.empty())
                    {
                        img.srvGpuHandle = 0;
                        img.texture      = Resource::TextureHandle{};
                    }
                    else
                    {
                        resolveUiImageHandle(img.texturePath, img.texture, img.srvGpuHandle);
                    }
                }

            if (UIPass* uip = renderer.GetUIPass())
            {
                m_uiSystem.Tick(world, m_uiInput, canvas,
                                uip->GetDrawList(), frameCtx.deltaTime);
                // Entity-as-widget Canvas UI feeds the same draw list, on top.
                m_uiCanvasSystem.Tick(world, m_uiInput, canvas,
                                      uip->GetDrawList(), frameCtx.deltaTime);
                UI::UISystem::MergeExternalCaptureMouse(
                    m_uiCanvasSystem.WantsCaptureMouse());
            }

            // World-space UI: per-frame fade/scale/cull + DamageNumber lifetimes.
            // WorldUIBillboardPass reads the same components using its own viewProj.
            UI::WorldSpaceUIView wsView;
            const RenderView&    rv = renderer.GetView();
            wsView.viewProjMatrix = rv.viewProjMatrixNoJitter;
            // Camera right / up = first 3 elements of view-matrix cols 0 / 1.
            wsView.cameraRightWS = { rv.viewMatrix.m[0][0],
                                      rv.viewMatrix.m[1][0],
                                      rv.viewMatrix.m[2][0] };
            wsView.cameraUpWS    = { rv.viewMatrix.m[0][1],
                                      rv.viewMatrix.m[1][1],
                                      rv.viewMatrix.m[2][1] };
            wsView.canvasSize = { canvas.size.x, canvas.size.y };
            wsView.valid      = true;
            m_worldSpaceUISystem.Tick(world, wsView, frameCtx.deltaTime);
        };

#ifdef WITH_EDITOR
        renderer.SetPickingOutlineEntity(m_editorLayer.GetSelectedEntity());
        // Push the editor's debug category toggles onto the renderer-owned
        // buckets BEFORE BeginFrame's wire-gather reads them. Editor-only:
        // Game builds never enable debug visuals.
        m_debugDraw.ApplyTo(renderer, m_navSystem);
        // Force the debug wire pass on (before BeginFrame runs Clear()) when the
        // selected entity has authored chain-physics groups to visualize.
        m_editorLayer.PrepareChainPhysicsOverlay(renderer, world);
#endif
        renderer.BeginFrame(world, ctx.frame, ctx.deltaTime,
                            ctx.viewportW, ctx.viewportH);

#ifdef WITH_EDITOR
        // Single editor-only debug submission point — emits the external
        // wireframes (collision mesh, navmesh) for enabled categories. Must run
        // AFTER BeginFrame (DebugWirePass ring slot is live) and BEFORE
        // renderer.Render (which uploads + draws the wire buffer).
        m_debugDraw.Submit(renderer, world, frameCtx, m_physicsSystem, m_navSystem);
        // Chain-physics (KawaiiPhysics-style) authoring overlay — root bone,
        // simulated chain bones + links, and excluded bones for the selection.
        m_editorLayer.EmitChainPhysicsOverlay(renderer, world);
#endif

        pumpUIInput();

        // Scene-transition fade/loading overlay — appended on top of the UI
        // draw list (bypasses ECS, so the new scene's world.Clear() can't wipe
        // it mid-fade). Consumed + cleared by the UIPass inside renderer.Render.
        if (UIPass* uip = renderer.GetUIPass())
            m_transition.DrawOverlay(uip->GetDrawList(), ctx.viewportW, ctx.viewportH);

        RHI::CommandList lastPassCL = renderer.Render();
        if (lastPassCL.IsValid())
            backend.AddCommandListDependency(primaryCL, lastPassCL);

        // Post-graph: composite to swap chain + ImGui.
        backend.SetRenderTargetToSwapChain(kClearColor, primaryCL);
        const uint64_t sceneTexId = renderer.GetFinalOutputSrvHandle()
                                    ? renderer.GetFinalOutputSrvHandle()
                                    : backend.GetHdrSceneSrvGpuHandle();
#ifdef WITH_EDITOR
        m_editorLayer.SetSceneTextureId(sceneTexId);
        m_editorLayer.SetViewportFullscreen(m_viewportFullscreen);
        m_editorLayer.SetWorld(ctx.world);
        m_editorLayer.SetRenderView(renderer.GetView());
        m_editorLayer.BeginImGuiFrame();
        m_editorLayer.OnUIRender();
        // Active mode's own ImGui hook (ShaderLab tool controls etc.).
        m_gameModeStack.OnUIRender(ctx);

        // Phase Debug — last-frame ms per system + per phase. App-side so
        // EngineCore stays ImGui-free. Toggled from Debug → Phase Debug.
        if (m_editorLayer.GetShowPhaseDebug())
        {
            bool open = true;
            if (ImGui::Begin("Phase Debug", &open))
            {
                const auto debugInfo = m_scheduler.GetDebugInfo();
                float totalMs = 0.f;
                for (const auto& p : debugInfo) totalMs += p.lastFrameMs;
                ImGui::Text("Scheduler total: %.3f ms", totalMs);
                ImGui::Separator();
                for (const auto& p : debugInfo)
                {
                    if (p.systems.empty()) continue;
                    const auto flags = ImGuiTreeNodeFlags_DefaultOpen
                                     | ImGuiTreeNodeFlags_SpanAvailWidth;
                    if (ImGui::TreeNodeEx(p.name, flags,
                                          "%-22s  %6.3f ms",
                                          p.name, p.lastFrameMs))
                    {
                        for (const auto& s : p.systems)
                            ImGui::Text("    %-30s  %6.3f ms",
                                        s.name, s.lastFrameMs);
                        ImGui::TreePop();
                    }
                }
            }
            ImGui::End();
            // Window X button → flip the menu toggle off so state stays synced.
            if (!open) m_editorLayer.SetShowPhaseDebug(false);
        }

        m_editorLayer.EndImGuiFrame(primaryCL);
#else
        // Game build: no ImGui — backend blits HDR scene to swap-chain RTV.
        backend.CompositeTextureToSwapChain(sceneTexId, primaryCL);
#endif

        backend.EndFrame();
    });

    m_scheduler.SetRegistry(&reg);
    reg.Initialize(m_world);
}

// ===========================================================================
// Run() helpers — per-frame
// ===========================================================================

void App::RefreshMainCamera()
{
    // Resolution order:
    //   1. The entity carrying ActiveCameraTag (set by Camera.SetActive),
    //      provided it's still alive and still has a CameraComponent.
    //   2. The previously-cached entity, if still valid.
    //   3. First-found-with-CameraComponent fallback (covers initial boot,
    //      world load before any Camera.SetActive is issued, and the case
    //      where the active camera entity was destroyed).
    Entity taggedActive = NullEntity;
    m_world.ForEach<ActiveCameraTag>([&](Entity e, ActiveCameraTag&)
    {
        if (taggedActive == NullEntity
            && m_world.IsAlive(e)
            && m_world.HasComponent<CameraComponent>(e))
        {
            taggedActive = e;
        }
    });
    if (taggedActive != NullEntity)
    {
        m_cameraEntity = taggedActive;
    }
    else if (m_cameraEntity == NullEntity
             || !m_world.IsAlive(m_cameraEntity)
             || !m_world.HasComponent<CameraComponent>(m_cameraEntity))
    {
        // Fallback: pick whichever camera-bearing entity comes first.
        m_cameraEntity = NullEntity;
        for (Entity e : m_world.GetEntities())
        {
            if (m_world.IsAlive(e) && m_world.HasComponent<CameraComponent>(e))
            { m_cameraEntity = e; break; }
        }
    }
    // Expose to Lua so BT/Logic scripts can read ctx:GetEntityPosition(CameraEntity).
    if (sol::state* lua = m_scriptSystem.GetLua())
        (*lua)["CameraEntity"] = static_cast<uint32_t>(m_cameraEntity);
}

void App::TickCamera(float dt, const GameModeContext& ctx)
{
    if (!m_world.GetComponent<CameraComponent>(m_cameraEntity)) return;

    // The camera is an ordinary ECS entity: pose on LocalTransform/GlobalTransform,
    // FPS state on CameraControllerComponent. Backfill anything missing.
    if (!m_world.HasComponent<LocalTransform>(m_cameraEntity))
        m_world.AddComponent<LocalTransform>(m_cameraEntity, LocalTransform{});
    if (!m_world.HasComponent<GlobalTransform>(m_cameraEntity))
        m_world.AddComponent<GlobalTransform>(m_cameraEntity, GlobalTransform{});
    if (!m_world.HasComponent<CameraControllerComponent>(m_cameraEntity))
        m_world.AddComponent<CameraControllerComponent>(m_cameraEntity, CameraControllerComponent{});

    // Backfill into the new VCam stack pipeline. Copies the legacy lens
    // params on first touch; subsequent edits via the Camera Controller
    // inspector / serialization still drive CameraComponent so the bridge
    // re-syncs each frame below.
    if (!m_world.HasComponent<VirtualCameraComponent>(m_cameraEntity))
    {
        VirtualCameraComponent vc{};
        if (auto* cam = m_world.GetComponent<CameraComponent>(m_cameraEntity))
        {
            vc.fov   = cam->fov;
            vc.nearZ = cam->nearZ;
            vc.farZ  = cam->farZ;
        }
        m_world.AddComponent<VirtualCameraComponent>(m_cameraEntity, vc);
    }
    if (!m_world.HasComponent<CameraPoseComponent>(m_cameraEntity))
        m_world.AddComponent<CameraPoseComponent>(m_cameraEntity, CameraPoseComponent{});
    if (!m_world.HasComponent<VCamPriorityComponent>(m_cameraEntity))
    {
        VCamPriorityComponent prio{};
        prio.priority = 0;       // baseline — gameplay VCams push at higher prio
        prio.weight   = 1.f;
        prio.enabled  = true;
        m_world.AddComponent<VCamPriorityComponent>(m_cameraEntity, std::move(prio));
    }
    if (!m_world.HasComponent<VCamBlendComponent>(m_cameraEntity))
    {
        VCamBlendComponent blend{};
        // Baseline starts already Active so first-frame resolve finds a winner.
        blend.currentBlend     = 1.f;
        blend.state            = BlendState::Active;
        blend.blendInDuration  = 0.f;
        blend.blendOutDuration = 0.25f;
        m_world.AddComponent<VCamBlendComponent>(m_cameraEntity, std::move(blend));
    }
    // Mirror legacy lens edits → VCam each frame so the inspector still
    // works as users expect. Cheap (3 float copies).
    if (auto* cam = m_world.GetComponent<CameraComponent>(m_cameraEntity))
    if (auto* vc  = m_world.GetComponent<VirtualCameraComponent>(m_cameraEntity))
    {
        vc->fov   = cam->fov;
        vc->nearZ = cam->nearZ;
        vc->farZ  = cam->farZ;
    }
    // Ensure the Main channel entity exists.
    Camera::GetOrCreateChannelEntity(m_world, Camera::kMainChannel);

    auto* ctrl = m_world.GetComponent<CameraControllerComponent>(m_cameraEntity);
    auto* lt   = m_world.GetComponent<LocalTransform>(m_cameraEntity);
    if (!ctrl || !lt) return;

    if (ctx.viewportRightMouseHeld)
    {
        m_cameraSystem.Update(*ctrl, *lt, ctx.mouseViewportDX,
                              ctx.mouseViewportDY, dt);
    }
    else
    {
        // Idle: script/turntable/gizmo/world-load may own the pose — keep
        // yaw/pitch tracking it so re-grabbing the camera doesn't snap.
        CameraSystem::SyncControllerFromTransform(*ctrl, *lt);
    }

    // Camera is a root entity — patch its GlobalTransform now so systems that
    // run before TransformSystem::Propagate (AI LOD, collision viz) see it.
    if (auto* gt = m_world.GetComponent<GlobalTransform>(m_cameraEntity))
        DirectX::XMStoreFloat4x4(&gt->matrix, lt->ToMatrix());
}

void App::UpdateViewportSize(GameModeContext& ctx, IGraphicsDevice& backend)
{
    if (m_viewportFullscreen)
    {
        ctx.viewportW = backend.GetWidth();
        ctx.viewportH = backend.GetHeight();
        return;
    }
#ifdef WITH_EDITOR
    unsigned int vpW = 0, vpH = 0;
    m_editorLayer.GetViewportSize(vpW, vpH);
    ctx.viewportW = vpW ? vpW : backend.GetWidth();
    ctx.viewportH = vpH ? vpH : backend.GetHeight();
#else
    ctx.viewportW = backend.GetWidth();
    ctx.viewportH = backend.GetHeight();
#endif
}

void App::RunFixedPhysicsLoop(float scaledDt, FrameContext& frameCtx)
{
    // T2: App owns the accumulator. PreAllSteps runs once for body create/
    // teleport detection, the Pre/Step/Post triplet N times (spiral-of-death
    // capped at 5 steps + 0.25s clamp), then PostAllSteps drains contacts.
    // physicsAlpha = leftover fraction for PhysicsInterpApplySystem's slerp.
    if (!frameCtx.runUpdate)
    {
        // Paused: zero the accumulator so resuming doesn't burn catch-up steps.
        m_physicsAccumulator = 0.f;
        frameCtx.physicsAlpha = 0.f;
        return;
    }

    m_physicsSystem.PreAllSteps(m_world);

    m_physicsAccumulator += scaledDt;
    constexpr float kMaxAccum = 0.25f;
    if (m_physicsAccumulator > kMaxAccum) m_physicsAccumulator = kMaxAccum;

    const float kFixedDt = DX12Physics::PhysicsSystem::GetFixedDt();
    int safety = 5;
    frameCtx.isFixedTickPhase = true;
    while (m_physicsAccumulator >= kFixedDt && safety-- > 0)
    {
        m_scheduler.RunPhase(TickPhase::FixedPhysicsPre,  m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::FixedPhysics,     m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::FixedPhysicsPost, m_world, frameCtx);
        m_physicsAccumulator -= kFixedDt;
        frameCtx.physicsStepIndex++;
    }
    frameCtx.isFixedTickPhase = false;

    m_physicsSystem.PostAllSteps(m_world);

    frameCtx.physicsAlpha = m_physicsAccumulator / kFixedDt;
    if (frameCtx.physicsAlpha < 0.f) frameCtx.physicsAlpha = 0.f;
    if (frameCtx.physicsAlpha > 1.f) frameCtx.physicsAlpha = 1.f;
}

void App::ShutdownAllSystems(IGraphicsDevice& backend)
{
    // Tear down ISystem adapters BEFORE the concrete singletons they wrap so
    // OnUnregister can still detach event listeners against live targets.
    m_systemRegistry.Shutdown(m_world);

    m_physicsSystem.Shutdown();

    // Audio: detach subscribers / entity-destroy listener before World tears
    // down; shutdown the clip system AFTER the engine releases voices because
    // VoiceRecord borrows AudioClipResource pointers until DestroyVoice flushes.
    m_audioSystem.Unbind();
    m_audioEngine.Shutdown();
    m_audioClipSys.Shutdown();

    // Explicit GPU-resource release while device is still alive.
    m_animClipSys.Shutdown();
    m_assetMgr.Shutdown();
    m_matSys.Shutdown(m_textureSys, backend);
    m_meshLib.Shutdown(backend);
    m_meshSys.Shutdown(backend);
    m_textureSys.Shutdown(backend);
    m_resourceMgr.Shutdown();
    Resource::BCCompressor::Get().Shutdown();
}
