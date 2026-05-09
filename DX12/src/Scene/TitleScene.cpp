#include "Scene/TitleScene.h"
#include "Scene/GameScene.h"
#include "ECS/Components.h"
#include "ECS/SkyboxComponent.h"
#include "Resource/WorldSerializer.h"
#include "Resource/AssetManager.h"
#include "Resource/AssetFS.h"
#include "Graphics/Renderer.h"
#include "System/Log.h"

#include <fstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    // Title's data lives in this .iworld — open it in Editor to add a logo,
    // tweak lighting, etc. without recompiling. The fallback below keeps the
    // engine bootable before the file exists.
    constexpr const char* kTitleWorldPath = "asset/scenes/title.iworld";

    bool TryLoadWorld(SceneContext& ctx, const char* path)
    {
        if (!::Resource::AssetFS::Get().HasInPak(path)
            && !std::ifstream(path).good())
            return false;
        if (!ctx.assetMgr) return false;
        std::string ppc;
        return ::Resource::LoadWorld(
            path, *ctx.world, *ctx.assetMgr,
            &ctx.renderer, /*animClipSys=*/nullptr,
            /*outName=*/nullptr, &ppc);
    }

    void SpawnFallback(World& world, std::vector<Entity>& tracked)
    {
        Entity cam = world.CreateEntity();
        world.SetName(cam, "Title Camera");
        CameraComponent cc;
        cc.position = { 0.f, 1.5f, -3.f };
        world.AddComponent<CameraComponent>(cam, cc);
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

void TitleScene::Init(SceneContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->world)
    {
        LOG_ERROR("TitleScene: Init received null SceneContext / World");
        return;
    }
    LOG_INFO("=== TITLE === press SPACE to start");

    World& world = *m_ctx->world;
    world.Clear();

    if (!TryLoadWorld(*m_ctx, kTitleWorldPath))
    {
        LOG_INFO("TitleScene: '%s' missing — using built-in fallback (open the "
                 "title.iworld file in Editor and Save World to author it)",
                 kTitleWorldPath);
        SpawnFallback(world, m_spawnedEntities);
    }
}

void TitleScene::Update(float /*dt*/)
{
    if (!m_ctx) return;

    // Edge-triggered SPACE: GetAsyncKeyState's low bit is set when a transition
    // happened since last call, so the request fires once per press.
    if ((GetAsyncKeyState(VK_SPACE) & 1) && m_ctx->requestReplaceScene)
    {
        LOG_INFO("TitleScene: -> GameScene");
        m_ctx->requestReplaceScene(std::make_unique<GameScene>());
    }
}

void TitleScene::Shutdown()
{
    if (!m_ctx || !m_ctx->world) return;
    World& world = *m_ctx->world;
    // If we LoadWorld'd, the next scene's Init will Clear() — no need to track
    // entities ourselves. The fallback path tracks via m_spawnedEntities.
    for (Entity e : m_spawnedEntities)
        if (world.IsAlive(e)) world.DestroyEntity(e);
    m_spawnedEntities.clear();
    LOG_INFO("TitleScene: Shutdown");
}
