#include "Scene/EndScene.h"
#include "Scene/TitleScene.h"
#include "ECS/Components.h"
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
    constexpr const char* kEndWorldPath = "asset/scenes/end.iworld";

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
        world.SetName(cam, "End Camera");
        world.AddComponent<CameraComponent>(cam, CameraComponent{});
        tracked.push_back(cam);
    }
}

void EndScene::Init(SceneContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->world)
    {
        LOG_ERROR("EndScene: Init received null SceneContext / World");
        return;
    }
    LOG_INFO("=== END === press ENTER to return to title");

    World& world = *m_ctx->world;
    world.Clear();

    if (!TryLoadWorld(*m_ctx, kEndWorldPath))
    {
        LOG_INFO("EndScene: '%s' missing — using built-in fallback (Save World "
                 "from Editor to author it)", kEndWorldPath);
        SpawnFallback(world, m_spawnedEntities);
    }
}

void EndScene::Update(float /*dt*/)
{
    if (!m_ctx) return;

    if ((GetAsyncKeyState(VK_RETURN) & 1) && m_ctx->requestReplaceScene)
    {
        LOG_INFO("EndScene: -> TitleScene");
        m_ctx->requestReplaceScene(std::make_unique<TitleScene>());
    }
}

void EndScene::Shutdown()
{
    if (!m_ctx || !m_ctx->world) return;
    World& world = *m_ctx->world;
    for (Entity e : m_spawnedEntities)
        if (world.IsAlive(e)) world.DestroyEntity(e);
    m_spawnedEntities.clear();
    LOG_INFO("EndScene: Shutdown");
}
