#include "App.h"
#include "System/Log.h"
#ifdef WITH_EDITOR
#include "imgui/imgui.h"
#endif
#include "System/Timer.h"
#include "System/FileWatcher.h"
#ifdef WITH_EDITOR
#include "Editor/EditorLayer.h"
#endif
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/Renderer.h"
#include "Graphics/RenderTypes.h"
#include "Scene/TestScene.h"
#include "Scene/TitleScene.h"
#include "Scene/GameScene.h"
#include "Scene/EndScene.h"
#include "Scene/ShaderLabScene.h"
#include "Scene/MeshSpawner.h"
#include "Scene/TransformSystem.h"
#include "ECS/Components.h"
#include "System/EventBus.h"
#include "AI/LuaBTBindings.h"
#include "Audio/AudioEvents.h"
#include "UI/UISystem.h"
#include "UI/UIComponents.h"
#include "UI/Font.h"
#include "UI/LuaUIBindings.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "System/Mouse.h"
#include "System/Keyboard.h"

// Resource system headers (for per-frame pumps and shutdown)
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "Resource/MeshSystem.h"
#include "Resource/MaterialSystem.h"
#include "Resource/BCCompressor.h"
#include "Resource/AssetFS.h"

// Loaders — internal format → Resource object
#include "Resource/TextureLoader.h"
#include "Resource/ShaderLoader.h"
#include "Resource/MaterialLoader.h"
#include "Audio/AudioClipLoader.h"

#ifdef WITH_EDITOR
// Importers — external source format → internal blob (editor-only;
// runtime only loads already-imported .itex/.imsh/.imat/.ianim/.aclip).
#include "Resource/TextureImporter.h"
#include "Resource/ShaderImporter.h"
#include "Resource/MaterialImporter.h"
#include "Resource/AnimationImporter.h"
#include "Resource/VmdImporter.h"
#include "Audio/AudioImporter.h"
#endif

// Animation resource system
#include "Resource/AnimationLoader.h"
#include "Resource/AnimationClipSystem.h"

static const UINT WindowWidth  = 1920;
static const UINT WindowHeight = 1080;

// Component editor registrations moved to EditorLayer::RegisterDefaultEditors().


App::App()
    : wnd(WindowWidth, WindowHeight, L"DX12 \u2014 Deferred Rendering")
{
    LOG_INFO("App initialized");
#ifdef WITH_EDITOR
    m_editorLayer.OnAttach();
#else
    // Game build: scene always fills the backbuffer.
    m_viewportFullscreen = true;
#endif
}

int App::Run()
{
    const float clearColor[4] = { 0.05f, 0.05f, 0.1f, 1.0f };

    // Mount game.ipak if present. Release ships with a pak (single sealed
    // archive, fast cold-start); Editor/dev runs ignore this and read loose
    // files off disk, which AssetFS::ReadFile falls back to automatically.
    ::Resource::AssetFS::Get().Mount("game.ipak");

    IGraphicsDevice& backend = wnd.Gfx();

    // ---- Register resource loaders (internal format → Resource) --------
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::TextureLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::ShaderLoader>(&backend));
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::MaterialLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::AnimationLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Audio::AudioClipLoader>());

#ifdef WITH_EDITOR
    // ---- Register importers (external source → internal blob) ----------
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::TextureImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::ShaderImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::MaterialImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::AnimationImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::VmdImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Audio::AudioImporter>());
#endif

    backend.InitPSOLibrary("pso_cache.bin");

    // ---- Initialize AssetManager + AnimationClipSystem ---------------------
    m_assetMgr.Init(m_meshSys, m_textureSys, m_resourceMgr, backend);
    m_animClipSys.Init(m_resourceMgr);

    // Bake the default UI font (FreeType → R8G8B8A8 atlas) and register it
    // as the global text provider so UIDrawList::AddText / TextWidget /
    // ButtonWidget labels render out of the box. Falls back silently to a
    // no-op text path if the TTF isn't on disk.
    {
        // FGMiraiRen ships with the engine; switch to any TTF asset path here.
        constexpr const char* kDefaultTTF = "asset/font/FGMiraiRen.ttf";
        if (UI::DefaultFont().Init(backend, kDefaultTTF, /*pixelSize*/ 24.f))
            UI::DefaultFont().InstallAsGlobal();
        else
            LOG_WARNING("UI font init failed — text rendering disabled");
    }

    // Wire UI mouse-capture into Window. When the cursor sits over a
    // hit-testable widget, ImGui-style camera drag / picking should not
    // fire — Window::ProcessMessage queries this on every input message.
    // ImGui already installs its own hook in editor builds; combine the two
    // via a Lambda forward so neither overrides the other.
#ifdef WITH_EDITOR
    {
        // Editor: ImGui already owns the hook (set in InitImGuiBackends).
        // The combined predicate is "ImGui wants OR UI wants".
        // Install AFTER InitImGuiBackends so we wrap the existing one.
        // (Done below, inside the editor wiring block.)
    }
#else
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

    // ---- Engine systems (App-owned; persist across scene switches) ---------
    // World, Script, Physics, Camera all live here. IScene::Init runs against
    // the World below via ctx.world; engine systems tick uniformly in the
    // main loop regardless of which IScene is active.
    m_scriptSystem.Initialize();
    m_scriptSystem.BindWorld(m_world);

    // BT runtime shares ScriptSystem's sol::state — single VM, hot-reload
    // and event bridges stay coherent. RegisterLuaBTBindings creates the
    // empty Actions / Conditions tables that .bt.lua scripts populate.
    if (sol::state* lua = m_scriptSystem.GetLua())
    {
        AI::RegisterLuaBTBindings(*lua);
        UI::RegisterLuaUIBindings(*lua, m_world);
        m_aiSystem.Init(lua);
    }

    m_physicsSystem.Init();

    // Audio. Initialise the XAudio2 engine + clip system first, then wire
    // the systems (events + per-frame ticks) so PlaySoundEvent / Stop /
    // SetParam work immediately. AudioSystem installs an entity-destroy
    // listener on the World so component teardown stops live voices and
    // releases clip refcounts.
    m_audioClipSys.Init(m_resourceMgr);
    if (m_audioEngine.Initialize())
    {
        m_audioSystem.BindEngine(m_audioEngine);
        m_audioSystem.BindClipSystem(m_audioClipSys);
        m_audioSystem.BindWorld(m_world);
        m_audio3DSystem.BindEngine(m_audioEngine);
    }

    // ---- Build SceneContext with all systems --------------------------------
    SceneContext ctx{ backend, renderer };
    ctx.resourceMgr = &m_resourceMgr;
    ctx.textureSys  = &m_textureSys;
    ctx.meshSys     = &m_meshSys;
    ctx.meshLib     = &m_meshLib;
    ctx.matSys      = &m_matSys;
    ctx.assetMgr    = &m_assetMgr;
    ctx.world       = &m_world;   // permanent — IScenes operate on this World

    // Scene transition queue — drained after SceneManager.Update each frame.
    std::unique_ptr<IScene> pendingScene;
    ctx.requestReplaceScene = [&pendingScene](std::unique_ptr<IScene> next) {
        pendingScene = std::move(next);
    };

    // Initial scene differs by build:
    //   ShaderLab → ShaderLabScene (sphere + 3-point lights + IBL; tool-only)
    //   Editor    → TestScene (default editing target; no state machine needed)
    //   Game      → TitleScene (full Title→Game→End flow; GameScene loads game.json)
#ifdef WITH_SHADERLAB
    m_sceneManager.PushScene(std::make_unique<ShaderLabScene>(), ctx);
#elif defined(WITH_EDITOR)
    m_sceneManager.PushScene(std::make_unique<TestScene>(), ctx);
#else
    m_sceneManager.PushScene(std::make_unique<TitleScene>(), ctx);
#endif

    // (game.json startup-world loading moved into GameScene::Init.)

#ifdef WITH_EDITOR
    // ---- Wire EditorLayer --------------------------------------------------
    m_editorLayer.SetGraphicsDevice(&backend);
    m_editorLayer.SetResourceSystems(&m_textureSys, &m_resourceMgr);
    m_editorLayer.SetAssetManager(&m_assetMgr);
    m_editorLayer.SetRenderer(&renderer);
    m_editorLayer.SetAnimationClipSystem(&m_animClipSys);
    m_editorLayer.SetAnimationSystem(renderer.GetAnimationSystem());
    m_editorLayer.RegisterDefaultEditors(); // must be after SetRenderer()
    m_editorLayer.SetAssetDirectory("asset/");
    m_editorLayer.SetGPUProfiler(&static_cast<GraphicsDX12&>(backend).GetGPUProfiler());
    m_editorLayer.InitImGuiBackends(); // must be after SetGraphicsDevice

    // EditorLayer installed an ImGui-only mouse-capture hook above. Replace
    // it with a combined predicate so Window also suppresses input when the
    // game-layer UI wants the cursor (button hover, etc.). Header note:
    // ImGui's WantCaptureMouse already covers editor chrome — the OR here
    // adds runtime UI on top.
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
        // px/py are already in viewport-panel space; PickingPass renders at the same render dims.
        renderer.RequestPick(static_cast<int>(px), static_cast<int>(py));
    });
#endif

    Timer timer;

    // ---- Shader hot-reload ------------------------------------------------
    // Watch shaders/ for .hlsl/.hlsli writes; when something changes, drop
    // the in-memory shader+PSO caches and have every reload-aware pass
    // re-fetch its PSOs. No-op in shipping builds — the watcher is
    // dev-friendly only, but it's cheap enough to leave on always.
    HotReload::FileWatcher shaderWatcher;
    shaderWatcher.Init("shaders");

    while (true)
    {
        // Hot-reload check: drain the file-change queue first thing each
        // frame. Doing it BEFORE BeginFrame means we haven't started
        // recording commands yet — FlushAndWait is the easy hammer to
        // ensure no in-flight CL still references the old PSOs we're
        // about to drop. v1 leans on the stall; if it ever shows up in
        // a profile, we'll switch to fence-tagged deferred deletion.
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
            m_sceneManager.PopScene();

            // Physics shutdown happens here (used to be in TestScene::Shutdown
            // but PhysicsSystem now lives in App).
            m_physicsSystem.Shutdown();

            // Audio: detach event subscribers + entity-destroy listener
            // before the World tears itself down, then destroy XAudio2
            // voices. Shutdown the clip system AFTER the engine releases
            // its voices — the engine's VoiceRecord still borrows
            // AudioClipResource pointers until DestroyVoice flushes.
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

            backend.SavePSOLibrary("pso_cache.bin");
            return *ecode;
        }

        const float dt = timer.Mark();

        // CPU frame timing (for profiler panel).
        LARGE_INTEGER cpuStart, cpuEnd, cpuFreq;
        QueryPerformanceCounter(&cpuStart);

#ifdef WITH_EDITOR
        if (GetAsyncKeyState(VK_F11) & 1)
            m_viewportFullscreen = !m_viewportFullscreen;
#endif

        // Resolve viewport size.
        if (m_viewportFullscreen)
        {
            ctx.viewportW = backend.GetWidth();
            ctx.viewportH = backend.GetHeight();
        }
        else
        {
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

#ifdef WITH_EDITOR
        // Forward camera-drag input from the previous frame's ImGui state.
        ctx.viewportRightMouseHeld = m_editorLayer.IsViewportRightDragging();
        m_editorLayer.GetViewportMouseDelta(ctx.mouseViewportDX, ctx.mouseViewportDY);
#else
        // Game build: no editor viewport; consume raw mouse state elsewhere if needed.
        ctx.viewportRightMouseHeld = false;
        ctx.mouseViewportDX = 0.f;
        ctx.mouseViewportDY = 0.f;
#endif

        // ---- Per-frame system pumps ----------------------------------------
        // Promote any async-loaded resources to GPU (budget: 2 ms per frame).
        m_resourceMgr.ProcessPendingGPUUploads(2.0f);
        // Upload dirty material CBVs.
        m_matSys.Tick(m_textureSys, backend);

        // Forward delta-time so renderer passes can scale temporal blending.
        ctx.deltaTime = dt;

        // ---- Camera (always — input feels broken if frozen during pause) ---
        // Refresh the "main camera" hint if it's stale (entity destroyed by
        // a scene pop, never set, or LoadWorld replaced everything).
        if (m_cameraEntity == NullEntity || !m_world.IsAlive(m_cameraEntity)
            || !m_world.HasComponent<CameraComponent>(m_cameraEntity))
        {
            m_cameraEntity = NullEntity;
            for (Entity e : m_world.GetEntities())
            {
                if (m_world.IsAlive(e) && m_world.HasComponent<CameraComponent>(e))
                { m_cameraEntity = e; break; }
            }
        }
        if (CameraComponent* cam = m_world.GetComponent<CameraComponent>(m_cameraEntity))
        {
            if (ctx.viewportRightMouseHeld)
                m_cameraSystem.Update(*cam, ctx.mouseViewportDX,
                                            ctx.mouseViewportDY, dt);

            RenderCamera rc;
            rc.position = cam->position;
            rc.yaw      = cam->yaw;
            rc.pitch    = cam->pitch;
            rc.fov      = cam->fov;
            rc.nearZ    = cam->nearZ;
            rc.farZ     = cam->farZ;
            renderer.SetCamera(rc);
        }

        // ---- Engine system ticks (gated by editor play/stop/step) ----------
        // Editor: Playing → real dt. Stopped/Paused → skip ticks unless the
        // user just clicked Step (one fixed 1/60s frame, deterministic).
        // Game build: no UI gate — always tick at real dt.
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
                // ShaderLab.exe ignores the editor's Stopped/Paused state —
                // there's no gameplay to pause, but tools-driven animation
                // (turntable, future timeline scrub, etc.) needs to keep
                // ticking. Editor.exe / Game.exe behaviour unchanged.
                else           runUpdate   = false;
#endif
            }
        }
#endif
        if (runUpdate)
        {
            // AfterDelay timers — REAL dt so hit-stop callbacks expire even
            // when m_timeScale is near zero.
            m_scriptSystem.TickTimers(effectiveDt);

            // Game time scale (Lua: Engine.SetTimeScale). 1=normal, 0=freeze.
            const float scaledDt = effectiveDt * m_scriptSystem.GetTimeScale();

            // Lua scripts (run before TransformSystem so script-driven transform
            // mutations propagate this same frame).
            m_scriptSystem.Update(m_world, scaledDt);
            m_scriptSystem.CheckHotReload(m_world);

            // BT AI runs after scripts (so scripts can stage perception data
            // into the blackboard) and before physics (so MoveDestination
            // entries written by BT actions are consumed the same frame).
            m_aiLODSystem.SetCameraEntity(m_cameraEntity);
            m_aiLODSystem.Update(m_world);
            m_aiSystem.CheckHotReload(m_world);
            m_aiSystem.Update(m_world, scaledDt);

            // Physics: reads LocalTransform, steps Jolt at fixed 60Hz, writes back.
            m_physicsSystem.Update(m_world, scaledDt);

            // Audio. AudioSystem first to drain queued PlaySound events and
            // sweep finished voices; Audio3DSystem then walks live 3D sources
            // with up-to-date GlobalTransform and pushes DSP into the engine.
            // Engine.Update reclaims voice slots flagged finished by the
            // XAudio2 worker thread.
            // Promote any RM-loaded .aclip resources to local Ready first so
            // events / playOnEnable that landed this frame can resolve.
            m_audioClipSys.Tick();
            EventBus::Get().DispatchOne<Audio::PlaySoundEvent>();
            EventBus::Get().DispatchOne<Audio::StopSoundEvent>();
            EventBus::Get().DispatchOne<Audio::SetAudioParamEvent>();
            EventBus::Get().DispatchOne<Audio::BusVolumeChangedEvent>();
            m_audioSystem  .Update(m_world, scaledDt);
            m_audio3DSystem.Update(m_world, scaledDt);
            m_audioEngine  .Update(scaledDt);

            // Per-scene gameplay hook — typically empty for the default level.
            m_sceneManager.Update(scaledDt);

            // Drain a scene transition request, if any. Doing this AFTER
            // Update guarantees we never mutate the stack while iterating it.
            if (pendingScene)
            {
                m_sceneManager.PopScene();
                m_sceneManager.PushScene(std::move(pendingScene), ctx);
                pendingScene.reset();
            }
        }

        // Propagate LocalTransform → GlobalTransform for every hierarchy entity.
        // Runs UNCONDITIONALLY (outside the play-state gate). The editor gizmo
        // writes to a single entity's LocalTransform and immediately patches
        // ITS own GlobalTransform; descendants only get refreshed by this
        // pass. Skipping it while paused leaves children visually pinned to
        // their old world positions when the user moves a parent — that was
        // why "moving the model root didn't move the bones / meshes".
        // Cost is trivial — pure derived-data math, no game state mutation.
        TransformSystem::Propagate(m_world);

        // Open the primary command list. BeginFrame does a pipelined wait (on
        // the fence from FrameCount frames ago) and drains that backbuffer's
        // deferred-release slot. Calls to Destroy{Buffer,Texture} /
        // ReleaseHdrRenderTarget / FreeDescriptorTable after this point queue
        // work for a future BeginFrame — always safe regardless of in-flight GPU.
        RHI::CommandList primaryCL = backend.BeginFrame();
        backend.SetViewportSize(ctx.viewportW, ctx.viewportH);

        // TextureSystem::Tick MUST come after BeginFrame (WaitForPreviousFrame):
        //   1. Flushes m_pendingDestroy — safe because previous frame is GPU-complete.
        //   2. Promotes RM-ready textures to GPU via CreateTexture/FlushUploadAndWait.
        m_textureSys.Tick(m_resourceMgr, backend);
        m_animClipSys.Tick();
        m_animClipSys.ResolvePendingBinds(m_world, renderer);

        // ---- UI tick — populates UIPass's drawlist before Render() ---------
        // Runs AFTER renderer.BeginFrame so world-space anchors project with
        // THIS frame's view-projection (no 1-frame lag when the camera moves).
        // Order: BeginFrame builds RenderView → UI Tick reads it → Render
        // consumes the drawlist inside UIPass.
        // Mouse position arrives in window-space from the engine input
        // singleton. In editor builds we offset by the viewport-panel top-left
        // so widgets hit-test against panel-relative pixels (matches where
        // they actually render inside the LDR target shown in the panel).
        // In game / fullscreen-viewport builds the panel min is (0,0) so the
        // translation is a no-op.
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
            m_uiInput.mouseMiddle = false; // engine Mouse has no middle bit yet

            // Drain typed text + key events so UI text fields receive input.
            // Skip when ImGui already grabbed the keyboard (its own widgets
            // are foreground); UI is one layer above the world, one below
            // the editor chrome, so we yield to ImGui first.
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
            m_uiInput.shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
            m_uiInput.ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            m_uiInput.alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;

            UI::UICanvas canvas;
            canvas.size = { static_cast<float>(ctx.viewportW),
                            static_cast<float>(ctx.viewportH) };

            // Screen-space UI tick.
            if (UIPass* uip = renderer.GetUIPass())
                m_uiSystem.Tick(m_world, m_uiInput, canvas,
                                uip->GetDrawList(), dt);

            // World-space UI tick — feeds per-frame fade/scale/cull into
            // WorldSpaceUIComponent and advances DamageNumber lifetimes.
            // WorldUIBillboardPass reads the same components later in the
            // graph using its own viewProj.
            UI::WorldSpaceUIView wsView;
            const RenderView&    rv = renderer.GetView();
            wsView.viewProjMatrix = rv.viewProjMatrixNoJitter;
            // Camera right / up extracted from the row-major view matrix —
            // first 3 elements of columns 0 (right) and 1 (up).
            wsView.cameraRightWS = { rv.viewMatrix.m[0][0],
                                      rv.viewMatrix.m[1][0],
                                      rv.viewMatrix.m[2][0] };
            wsView.cameraUpWS    = { rv.viewMatrix.m[0][1],
                                      rv.viewMatrix.m[1][1],
                                      rv.viewMatrix.m[2][1] };
            wsView.canvasSize = { canvas.size.x, canvas.size.y };
            wsView.valid      = true;
            m_worldSpaceUISystem.Tick(m_world, wsView, dt);
        };

        // Render — always runs, regardless of pause state. ECS → DrawPackets,
        // then RenderGraph passes each emit their own command list.
        renderer.BeginFrame(m_world, ctx.frame, ctx.deltaTime,
                            ctx.viewportW, ctx.viewportH);
        // UI tick runs HERE (after BeginFrame so RenderView is current) but
        // BEFORE Render so UIPass picks up the freshly-populated drawlist.
        pumpUIInput();
        RHI::CommandList lastPassCL = renderer.Render();
        if (lastPassCL.IsValid())
            backend.AddCommandListDependency(primaryCL, lastPassCL);

        // Post-graph: composite to swap chain + ImGui.
        backend.SetRenderTargetToSwapChain(clearColor, primaryCL);
        // Use the tone-mapped final output when available; fall back to raw HDR.
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
        // Active scene gets its own ImGui hook — used by ShaderLabScene to
        // render mesh / lights / HDRI / turntable controls without polluting
        // EditorLayer with tool-specific code (design doc §6.3).
        m_sceneManager.OnUIRender(ctx);
        m_editorLayer.EndImGuiFrame(primaryCL);
#else
        // Game build: no editor UI, no ImGui — backend's own shader path does
        // the fullscreen scene blit directly to the swap-chain RTV that
        // SetRenderTargetToSwapChain just bound.
        backend.CompositeTextureToSwapChain(sceneTexId, primaryCL);
#endif

        backend.EndFrame();

        // Update CPU frame time for profiler.
        QueryPerformanceCounter(&cpuEnd);
        QueryPerformanceFrequency(&cpuFreq);
#ifdef WITH_EDITOR
        m_editorLayer.SetCPUFrameTime(
            static_cast<float>(cpuEnd.QuadPart - cpuStart.QuadPart) * 1000.0f
            / static_cast<float>(cpuFreq.QuadPart));

        // Resolve any pending GPU pick (1-frame delay).
        {
            Entity pickedEntity;
            if (renderer.ResolvePick(pickedEntity))
                m_editorLayer.SetPickResult(pickedEntity);
        }
#else
        (void)cpuEnd; (void)cpuFreq;
#endif

        ++ctx.frame;
    }
}
