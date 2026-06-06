#include "Scene/TitleScene.h"
#include "Scene/GameScene.h"
#include "ECS/Components.h"
#include "ECS/CameraSystem.h"
#include "ECS/SkyboxComponent.h"
#include "Resource/SceneSerializer.h"
#include "Resource/AssetManager.h"
#include "Resource/AssetFS.h"
#include "Graphics/Renderer.h"
#include "Input/InputSystem.h"
#include "System/Log.h"

#include <fstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    // Title's data lives in this .iscene — open it in Editor to add a logo,
    // tweak lighting, etc. without recompiling. The fallback below keeps the
    // engine bootable before the file exists.
    constexpr const char* kTitleScenePath = "asset/scenes/title.iscene";

    bool TryLoadScene(GameModeContext& ctx, const char* path)
    {
        if (!::Resource::AssetFS::Get().HasInPak(path)
            && !std::ifstream(path).good())
            return false;
        if (!ctx.assetMgr) return false;
        std::string ppc;
        return ::Resource::LoadScene(
            path, *ctx.world, *ctx.assetMgr,
            &ctx.renderer, /*animClipSys=*/nullptr,
            /*outName=*/nullptr, &ppc);
    }

    void SpawnFallback(World& world, std::vector<Entity>& tracked)
    {
        Entity cam = world.CreateEntity();
        world.SetName(cam, "Title Camera");
        CameraControllerComponent camCtrl{};
        world.AddComponent<CameraComponent>(cam, CameraComponent{});
        world.AddComponent<CameraControllerComponent>(cam, camCtrl);
        world.AddComponent<LocalTransform>(cam, CameraSystem::MakeTransform(camCtrl, { 0.f, 1.5f, -3.f }));
        world.AddComponent<GlobalTransform>(cam, GlobalTransform{});
        tracked.push_back(cam);

        Entity light = world.CreateEntity();
        world.SetName(light, "Title Light");
        LightData ld;
        ld.direction = { 0.447f, -0.894f, 0.224f };
        ld.color     = { 1.f, 0.92f, 0.82f };
        ld.intensity = 1.f;
        ld.type      = LightType::Directional;
        world.AddComponent<LightData>(light, ld);
        tracked.push_back(light);

        Entity sky = world.CreateEntity();
        world.SetName(sky, "Title Skybox");
        SkyboxComponent sc;
        sc.irradiancePath  = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Irradiance.itex";
        sc.radiancePath    = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Radiance.itex";
        sc.skyboxPath      = "asset/IBL/autumn_field_puresky/autumn_field_puresky_skybox.itex";
        sc.radianceMipLevels = 7;
        sc.iblStrength     = 1.0f;
        world.AddComponent<SkyboxComponent>(sky, sc);
        tracked.push_back(sky);
    }
}

void TitleScene::Init(GameModeContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->world)
    {
        LOG_ERROR("TitleScene: Init received null GameModeContext / World");
        return;
    }
    LOG_INFO("=== TITLE === press SPACE to start");

    World& world = *m_ctx->world;
    world.Clear();

    if (!TryLoadScene(*m_ctx, kTitleScenePath))
    {
        LOG_INFO("TitleScene: '%s' missing — using built-in fallback (open the "
                 "title.iscene file in Editor and Save Scene to author it)",
                 kTitleScenePath);
        SpawnFallback(world, m_spawnedEntities);
    }
}

void TitleScene::Update(float /*dt*/)
{
    if (!m_ctx) return;

    if (Input::Get().WasKeyPressed(VK_SPACE))
    {
        LOG_INFO("TitleScene: -> GameScene");
        if (m_ctx->beginTransition)
            m_ctx->beginTransition(std::make_unique<GameScene>());
        else if (m_ctx->requestReplaceMode)
            m_ctx->requestReplaceMode(std::make_unique<GameScene>());
    }
}

void TitleScene::Shutdown()
{
    if (!m_ctx || !m_ctx->world) return;
    World& world = *m_ctx->world;
    // If we LoadScene'd, the next mode's Init will Clear() — no need to track
    // entities ourselves. The fallback path tracks via m_spawnedEntities.
    for (Entity e : m_spawnedEntities)
        if (world.IsAlive(e)) world.DestroyEntity(e);
    m_spawnedEntities.clear();
    LOG_INFO("TitleScene: Shutdown");
}
