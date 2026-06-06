#include "Scene/EndScene.h"
#include "Scene/TitleScene.h"
#include "ECS/Components.h"
#include "ECS/CameraSystem.h"
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
    constexpr const char* kEndScenePath = "asset/scenes/end.iscene";

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
        world.SetName(cam, "End Camera");
        CameraControllerComponent camCtrl{};
        world.AddComponent<CameraComponent>(cam, CameraComponent{});
        world.AddComponent<CameraControllerComponent>(cam, camCtrl);
        world.AddComponent<LocalTransform>(cam, CameraSystem::MakeTransform(camCtrl, { 4.f, 3.f, 5.f }));
        world.AddComponent<GlobalTransform>(cam, GlobalTransform{});
        tracked.push_back(cam);
    }
}

void EndScene::Init(GameModeContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->world)
    {
        LOG_ERROR("EndScene: Init received null GameModeContext / World");
        return;
    }
    LOG_INFO("=== END === press ENTER to return to title");

    World& world = *m_ctx->world;
    world.Clear();

    if (!TryLoadScene(*m_ctx, kEndScenePath))
    {
        LOG_INFO("EndScene: '%s' missing — using built-in fallback (Save Scene "
                 "from Editor to author it)", kEndScenePath);
        SpawnFallback(world, m_spawnedEntities);
    }
}

void EndScene::Update(float /*dt*/)
{
    if (!m_ctx) return;

    if (Input::Get().WasKeyPressed(VK_RETURN))
    {
        LOG_INFO("EndScene: -> TitleScene");
        if (m_ctx->beginTransition)
            m_ctx->beginTransition(std::make_unique<TitleScene>());
        else if (m_ctx->requestReplaceMode)
            m_ctx->requestReplaceMode(std::make_unique<TitleScene>());
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
