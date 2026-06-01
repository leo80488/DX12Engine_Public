#include "Scene/GameScene.h"
#include "Scene/EndScene.h"
#include "Scene/MeshSpawner.h"
#include "ECS/Components.h"
#include "ECS/CameraSystem.h"
#include "ECS/SkyboxComponent.h"
#include "Resource/SceneSerializer.h"
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"
#include "Graphics/Renderer.h"
#include "Input/InputSystem.h"
#include "System/Log.h"

#include "Resource/AssetFS.h"

#include <nlohmann/json.hpp>
#include <fstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    // Built-in default if neither game.json nor a manifest override applies.
    // Author this in Editor (File → Save Scene) to get a data-driven default
    // GameScene without needing a game.json.
    constexpr const char* kDefaultGameScene = "asset/scenes/game.iscene";

    bool SceneExists(const std::string& path)
    {
        if (path.empty()) return false;
        if (::Resource::AssetFS::Get().HasInPak(path)) return true;
        std::ifstream f(path);
        return f.good();
    }

    // Read game.json's "startup_scene" string. Returns "" if file missing,
    // not parseable, or field absent / empty.
    std::string ReadStartupScene(const char* manifestPath)
    {
        std::ifstream f(manifestPath);
        if (!f) return {};
        try
        {
            nlohmann::json j;
            f >> j;
            if (j.contains("startup_scene") && j["startup_scene"].is_string())
                return j["startup_scene"].get<std::string>();
        }
        catch (const std::exception& e)
        {
            LOG_WARNING("GameScene: failed to parse %s: %s", manifestPath, e.what());
        }
        return {};
    }

    // Spawn the engine's default sandbox content (used when no startup_world
    // is configured). Mirrors what TestScene::Init does.
    void SpawnDefaults(World& world, std::vector<Entity>& tracked)
    {
        Entity cam = world.CreateEntity();
        world.SetName(cam, "Main Camera");
        CameraControllerComponent camCtrl{};
        world.AddComponent<CameraComponent>(cam, CameraComponent{});
        world.AddComponent<CameraControllerComponent>(cam, camCtrl);
        world.AddComponent<LocalTransform>(cam, CameraSystem::MakeTransform(camCtrl, { 4.f, 3.f, 5.f }));
        world.AddComponent<GlobalTransform>(cam, GlobalTransform{});
        tracked.push_back(cam);

        Entity light = world.CreateEntity();
        world.SetName(light, "Directional Light");
        LightData ld;
        ld.direction = { 0.447f, -0.894f, 0.224f };
        ld.color     = { 1.f, 0.92f, 0.82f };
        ld.intensity = 1.f;
        ld.type      = LightType::Directional;
        world.AddComponent<LightData>(light, ld);
        tracked.push_back(light);

        Entity sky = world.CreateEntity();
        world.SetName(sky, "Skybox");
        SkyboxComponent sc;
        sc.irradiancePath  = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Irradiance.itex";
        sc.radiancePath    = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Radiance.itex";
        sc.skyboxPath      = "asset/IBL/autumn_field_puresky/autumn_field_puresky_skybox.itex";
        sc.radianceMipLevels = 7;
        sc.iblStrength     = 1.0f;
        world.AddComponent<SkyboxComponent>(sky, sc);
        tracked.push_back(sky);

        // Default cube (not tracked — let it survive scene shutdown for the
        // demo; if you want it cleaned up too, push_back its entity here).
        MeshSpawner::Spawn(0, world);
    }
}

void GameScene::Init(GameModeContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->world)
    {
        LOG_ERROR("GameScene: Init received null GameModeContext / World");
        return;
    }
    LOG_INFO("=== GAME === press Q to end");

    World& world = *m_ctx->world;

    // Fresh slate — Title's entities (or the previous game's) get wiped.
    world.Clear();

    // Resolve the scene to load, in priority order:
    //   1. game.json "startup_scene"
    //   2. asset/scenes/game.iscene (engine convention)
    //   3. built-in hardcoded defaults
    std::string scenePath = ReadStartupScene("game.json");
    if (scenePath.empty() && SceneExists(kDefaultGameScene))
        scenePath = kDefaultGameScene;

    if (!scenePath.empty() && m_ctx->assetMgr != nullptr)
    {
        LOG_INFO("GameScene: loading scene '%s'", scenePath.c_str());
        std::string ppcPath;
        const bool ok = Resource::LoadScene(
            scenePath, world, *m_ctx->assetMgr,
            &m_ctx->renderer, /*animClipSys=*/nullptr,
            /*outName=*/nullptr, &ppcPath);
        if (!ok)
        {
            LOG_ERROR("GameScene: LoadScene('%s') failed — falling back to defaults",
                      scenePath.c_str());
            SpawnDefaults(world, m_spawnedEntities);
        }
        else
        {
            m_loadedFromManifest = true;
        }
    }
    else
    {
        LOG_INFO("GameScene: no startup_scene / asset/scenes/game.iscene — using defaults");
        SpawnDefaults(world, m_spawnedEntities);
    }
}

void GameScene::Update(float /*dt*/)
{
    if (!m_ctx) return;

    // Placeholder game-over trigger: press Q. Replace with real win/lose
    // detection (player HP <= 0, boss defeated, timer expired, …).
    if (Input::Get().WasKeyPressed('Q') && m_ctx->requestReplaceMode)
    {
        LOG_INFO("GameScene: -> EndScene");
        m_ctx->requestReplaceMode(std::make_unique<EndScene>());
    }
}

void GameScene::Shutdown()
{
    if (!m_ctx || !m_ctx->world) return;
    World& world = *m_ctx->world;

    if (m_loadedFromManifest)
    {
        // World was rebuilt by LoadScene — easiest is to wipe it; the next
        // mode starts clean and its Init populates as it sees fit.
        world.Clear();
    }
    else
    {
        for (Entity e : m_spawnedEntities)
            if (world.IsAlive(e)) world.DestroyEntity(e);
    }
    m_spawnedEntities.clear();
    LOG_INFO("GameScene: Shutdown");
}
