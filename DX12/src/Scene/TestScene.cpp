#include "Scene/TestScene.h"
#include "ECS/Components.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/TerrainComponent.h"
#include "ECS/GrassComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/CameraSystem.h"
#include "ECS/VideoComponent.h"
#include "ECS/VideoHelpers.h"
#include "ECS/Mp4FrameSource.h"
#include "Scene/MeshSpawner.h"
#include "Scripting/ScriptComponent.h"
#include "System/Log.h"

void TestScene::Init(GameModeContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->world)
    {
        LOG_ERROR("TestScene: Init received null GameModeContext / World");
        return;
    }
    LOG_INFO("TestScene: Init");

    World& world = *m_ctx->world;

    // Camera entity — App keeps a "main camera" hint pointing at the first
    // entity it finds carrying CameraComponent, so we just need to ensure
    // there is one. Subsequent scenes can override by spawning their own.
    // Pose lives on LocalTransform; FPS-controller state on CameraController.
    Entity cam = world.CreateEntity();
    world.SetName(cam, "Main Camera");
    CameraControllerComponent camCtrl{};
    // Perch above the terrain demo looking across the valley lake (terrain
    // median surface sits around y≈+3 with the tuning below).
    camCtrl.yaw       = 3.1416f;   // facing -Z
    camCtrl.pitch     = 0.42f;     // gentle look-down
    camCtrl.moveSpeed = 25.0f;     // the tile is 1 km — fly faster
    CameraComponent camLens{};
    camLens.farZ = 1500.0f;        // see the whole 1 km tile + sky
    world.AddComponent<CameraComponent>(cam, camLens);
    world.AddComponent<CameraControllerComponent>(cam, camCtrl);
    world.AddComponent<LocalTransform>(cam, CameraSystem::MakeTransform(camCtrl, { 0.f, 26.f, 55.f }));
    world.AddComponent<GlobalTransform>(cam, GlobalTransform{});
    m_spawnedEntities.push_back(cam);

    // Directional light
    Entity lightEnt = world.CreateEntity();
    world.SetName(lightEnt, "Directional Light");
    LightData ld;
    ld.direction = { 0.447f, -0.894f, 0.224f };
    ld.color     = { 1.f, 0.92f, 0.82f };
    ld.intensity = 1.f;
    ld.type      = LightType::Directional;
    world.AddComponent<LightData>(lightEnt, ld);
    m_spawnedEntities.push_back(lightEnt);

    // Skybox + IBL environment
    Entity skyboxEnt = world.CreateEntity();
    world.SetName(skyboxEnt, "Skybox");
    SkyboxComponent sc;
    sc.irradiancePath  = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Irradiance.itex";
    sc.radiancePath    = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Radiance.itex";
    sc.skyboxPath      = "asset/IBL/autumn_field_puresky/autumn_field_puresky_skybox.itex";
    sc.radianceMipLevels = 7;
    sc.iblStrength     = 1.0f;
    world.AddComponent<SkyboxComponent>(skyboxEnt, sc);
    m_spawnedEntities.push_back(skyboxEnt);

    // ---- Terrain + Grass + Water demo ---------------------------------------
    // One 1 km heightmap tile with 4 auto-blended PBR layers, a GoT-style
    // procedural grass field anchored to it, and a valley lake.
    //
    // Height tuning: the Renderer re-anchors the heightmap's actual data
    // range, so worldCenter.y IS the valley floor and heightScale IS the
    // total relief (floor → peak), scale-invariant pivot:
    //   surface Y ∈ [-12, +26], median ≈ +2
    //   water level  -2.0  → floods the lowest ~20% (valley lake)
    //   grass band   -1.4 .. +14, slopes < 38°
    //   rock layer   takes over on cliffs; pebbles around the waterline.
    constexpr float kTerrainSize  = 1024.0f;
    constexpr float kHeightScale  = 38.0f;    // TOTAL relief: floor → peak
    constexpr float kTerrainBaseY = -12.0f;   // valley floor (pinned)
    constexpr float kWaterLevel   = -2.0f;
    //{
    //    Entity terrainEnt = world.CreateEntity();
    //    world.SetName(terrainEnt, "Terrain");
    //    TerrainComponent tc;
    //    tc.heightmapPath = "asset/EngineResource/Terrain/HeightMap.itex";
    //    tc.worldCenter   = { 0.0f, kTerrainBaseY, 0.0f };
    //    tc.worldSize     = kTerrainSize;
    //    tc.heightScale   = kHeightScale;
    //    tc.tilesPerSide  = 128;            // 8 m sub-tiles, ~0.73 m quads
    //    // Height-correlated blend: disp maps sharpen the layer transitions.
    //    tc.heightBlendEnabled  = true;
    //    tc.heightBlendStrength = 0.15f;
    //    tc.heightBlendRange    = 0.10f;

    //    const char* kTerrainDir = "asset/EngineResource/Terrain/";

    //    // Layer 0 — grass+rock ground cover (the broad mid band, gentle slope)
    //    tc.layers[0].albedoPath    = std::string(kTerrainDir) + "aerial_grass_rock_diff_1k.itex";
    //    tc.layers[0].normalPath    = std::string(kTerrainDir) + "aerial_grass_rock_nor_dx_1k.itex";
    //    tc.layers[0].armPath       = std::string(kTerrainDir) + "aerial_grass_rock_arm_1k.itex";
    //    tc.layers[0].dispPath      = std::string(kTerrainDir) + "aerial_grass_rock_disp_1k.itex";
    //    tc.layers[0].tilingScale   = 0.50f;
    //    tc.layers[0].minHeight     =  -3.0f;
    //    tc.layers[0].maxHeight     =  16.0f;
    //    tc.layers[0].fadeHeight    =   4.0f;
    //    tc.layers[0].minSlopeDeg   =   0.0f;
    //    tc.layers[0].maxSlopeDeg   =  40.0f;
    //    tc.layers[0].fadeSlopeDeg  =   8.0f;

    //    // Layer 1 — rocks on cliffs (any altitude, steep slope)
    //    tc.layers[1].albedoPath    = std::string(kTerrainDir) + "aerial_rocks_02_diff_1k.itex";
    //    tc.layers[1].normalPath    = std::string(kTerrainDir) + "aerial_rocks_02_nor_dx_1k.itex";
    //    tc.layers[1].armPath       = std::string(kTerrainDir) + "aerial_rocks_02_arm_1k.itex";
    //    tc.layers[1].dispPath      = std::string(kTerrainDir) + "aerial_rocks_02_disp_1k.itex";
    //    tc.layers[1].tilingScale   = 0.50f;
    //    tc.layers[1].minHeight     = -10000.0f;
    //    tc.layers[1].maxHeight     =  10000.0f;
    //    tc.layers[1].fadeHeight    =     10.0f;
    //    tc.layers[1].minSlopeDeg   =  32.0f;
    //    tc.layers[1].maxSlopeDeg   =  90.0f;
    //    tc.layers[1].fadeSlopeDeg  =  10.0f;

    //    // Layer 2 — river pebbles around / below the waterline
    //    tc.layers[2].albedoPath    = std::string(kTerrainDir) + "ganges_river_pebbles_diff_1k.itex";
    //    tc.layers[2].normalPath    = std::string(kTerrainDir) + "ganges_river_pebbles_nor_dx_1k.itex";
    //    tc.layers[2].armPath       = std::string(kTerrainDir) + "ganges_river_pebbles_arm_1k.itex";
    //    tc.layers[2].dispPath      = std::string(kTerrainDir) + "ganges_river_pebbles_disp_1k.itex";
    //    tc.layers[2].tilingScale   = 0.50f;
    //    tc.layers[2].minHeight     = -10000.0f;
    //    tc.layers[2].maxHeight     = kWaterLevel + 1.0f;
    //    tc.layers[2].fadeHeight    =     2.5f;
    //    tc.layers[2].minSlopeDeg   =   0.0f;
    //    tc.layers[2].maxSlopeDeg   =  30.0f;
    //    tc.layers[2].fadeSlopeDeg  =   8.0f;

    //    // Layer 3 — rocky ground on the high tops
    //    tc.layers[3].albedoPath    = std::string(kTerrainDir) + "rocks_ground_06_diff_1k.itex";
    //    tc.layers[3].normalPath    = std::string(kTerrainDir) + "rocks_ground_06_nor_dx_1k.itex";
    //    tc.layers[3].armPath       = std::string(kTerrainDir) + "rocks_ground_06_arm_1k.itex";
    //    tc.layers[3].dispPath      = std::string(kTerrainDir) + "rocks_ground_06_disp_1k.itex";
    //    tc.layers[3].tilingScale   = 0.50f;
    //    tc.layers[3].minHeight     =  14.0f;
    //    tc.layers[3].maxHeight     = 10000.0f;
    //    tc.layers[3].fadeHeight    =    5.0f;
    //    tc.layers[3].minSlopeDeg   =   0.0f;
    //    tc.layers[3].maxSlopeDeg   =  45.0f;
    //    tc.layers[3].fadeSlopeDeg  =  10.0f;

    //    world.AddComponent(terrainEnt, tc);
    //    m_spawnedEntities.push_back(terrainEnt);
    //}

    //// Grass chunks — procedural blades over the whole tile, gated to the
    //// band above the waterline and below the rocky tops, never on cliffs.
    //{
    //    Entity grassEnt = world.CreateEntity();
    //    world.SetName(grassEnt, "Grass Field");
    //    GrassComponent gc;
    //    gc.worldCenter    = { 0.0f, kTerrainBaseY, 0.0f };  // y only used w/o terrain
    //    gc.worldSize      = kTerrainSize;
    //    gc.patchesPerSide = 256;                  // 4 m patches
    //    gc.density        = 8.0f;
    //    gc.lod0Dist       = 24.0f;
    //    gc.lod1Dist       = 64.0f;
    //    gc.cullDist       = 140.0f;
    //    gc.minWorldY      = kWaterLevel + 0.6f;   // stop just above the shoreline
    //    gc.maxWorldY      = 14.0f;                // below the rocky tops
    //    gc.maxSlopeDeg    = 38.0f;
    //    world.AddComponent(grassEnt, gc);
    //    m_spawnedEntities.push_back(grassEnt);
    //}

    //// Valley lake — Fresnel sky reflection + flow normals; depth/shore fade
    //// computed analytically from the terrain heightmap.
    //{
    //    Entity waterEnt = world.CreateEntity();
    //    world.SetName(waterEnt, "Water");
    //    WaterComponent wc;
    //    wc.worldCenter = { 0.0f, kWaterLevel, 0.0f };
    //    wc.worldSize   = kTerrainSize;
    //    world.AddComponent(waterEnt, wc);
    //    m_spawnedEntities.push_back(waterEnt);
    //}


    // Default cube at origin — MeshSpawner returns the new entity in the
    // currently-active world; we don't track it here because gameplay tests
    // typically want it to persist through scene-pop (this is the demo cube).
    MeshSpawner::Spawn(0, world);

    // ---- MULTI-SCRIPT demo --------------------------------------------------
    // One cube driven by TWO independent Lua Logic scripts at once: Bob.lua
    // oscillates it vertically while Spin.lua yaws it. Each slot gets its own
    // Lua instance, OnSpawn/OnUpdate lifecycle, and exposed-variable overrides
    // — exercising ScriptComponent's new std::vector<ScriptInstance> list. Open
    // the Script component in the Inspector to add/remove/tweak either slot.
    {
        Entity demo = MeshSpawner::Spawn(0, world);   // 0 = Cube
        if (demo != NullEntity)
        {
            world.SetName(demo, "Multi-Script Cube");
            if (LocalTransform* lt = world.GetComponent<LocalTransform>(demo))
                lt->translation = { 2.5f, 0.0f, 0.0f };   // beside the default cube

            ScriptComponent sc;
            {
                ScriptInstance bob;
                bob.scriptPath = "asset/scripts/logic/demo/Bob.lua";
                bob.enabled    = true;
                sc.scripts.push_back(std::move(bob));
            }
            {
                ScriptInstance spin;
                spin.scriptPath = "asset/scripts/logic/demo/Spin.lua";
                spin.enabled    = true;
                sc.scripts.push_back(std::move(spin));
            }
            world.AddComponent<ScriptComponent>(demo, std::move(sc));

            m_spawnedEntities.push_back(demo);
            LOG_SUCCESS("TestScene: spawned 'Multi-Script Cube' with %zu scripts (Bob + Spin)",
                        world.GetComponent<ScriptComponent>(demo)->scripts.size());
        }
    }

    // ---- TEST: world-space video plane ------------------------------------
    // Spawns a 2 m × 2 m quad standing next to the demo cube and uses a
    // SMPTE-75 % colour-bar NV12 texture as its surface. Exercises:
    //   - NV12 dual-plane SRV (Y + UV planes)
    //   - VideoQuadPass world-transform + depth-tested alpha-blend draw
    //   - Renderer scan / cross-queue dependency wiring
    // Replace LoadTestPattern with a real `VideoComponent::frameSource`
    // once a demuxer ships to play actual video on the plane.
    //{
    //    Entity videoEnt = world.CreateEntity();
    //    world.SetName(videoEnt, "Video Plane");

    //    VideoComponent vc{};
    //    vc.worldSpace  = true;
    //    vc.worldWidth  = 2.0f;
    //    vc.worldHeight = 2.0f;
    //    vc.renderAlpha = 1.0f;
    //    vc.colorSpace  = 0;       // BT.709
    //    world.AddComponent<VideoComponent>(videoEnt, std::move(vc));

    //    // Place the quad two metres to the right of the demo cube, lifted
    //    // a bit so the centre is roughly at eye height with the default
    //    // camera (eye = (4, 3, 5)). Quad faces +Z in local space; no
    //    // rotation needed for a head-on view from the +Z-side camera.
    //    using namespace DirectX;
    //    LocalTransform lt{};
    //    lt.translation = { 2.0f, 1.0f, 0.0f };
    //    lt.rotation    = { 0.0f, 0.0f, 0.0f, 1.0f };
    //    lt.scale       = { 1.0f, 1.0f, 1.0f };
    //    world.AddComponent<LocalTransform>(videoEnt, lt);

    //    GlobalTransform gt{};
    //    XMStoreFloat4x4(&gt.matrix,
    //        XMMatrixTranslation(lt.translation.x, lt.translation.y, lt.translation.z));
    //    world.AddComponent<GlobalTransform>(videoEnt, gt);

    //    if (Video::LoadTestPattern(m_ctx->gfx,
    //                                *world.GetComponent<VideoComponent>(videoEnt),
    //                                256, 256))
    //    {
    //        LOG_SUCCESS("TestScene: video plane ready at (2, 1, 0), 2 m × 2 m "
    //                    "with SMPTE colour bars");
    //    }
    //    m_spawnedEntities.push_back(videoEnt);
    //}

    //// ---- TEST: mp4 video plane via FFmpeg --------------------------------
    //// Drop a file at `asset/test.mp4` (or .mkv / .mov) and it will play on a
    //// 16:9 plane next to the colour-bar plane. Missing file → just skipped
    //// (the SMPTE plane still shows so you can verify the rest of the
    //// pipeline). Plays on loop; default speed 1.0; uses BT.709.
    //{
    //    auto src = Video::OpenMp4(m_ctx->gfx, "asset/test.mp4");
    //    if (src)
    //    {
    //        Entity mp4Ent = world.CreateEntity();
    //        world.SetName(mp4Ent, "Mp4 Video Plane");

    //        VideoComponent vc{};
    //        vc.worldSpace        = true;
    //        vc.worldWidth        = 4.0f;        // 16:9 plane, 4 m wide
    //        vc.worldHeight       = 4.0f * 9.0f / 16.0f;
    //        vc.renderAlpha       = 1.0f;
    //        vc.colorSpace        = 0;           // BT.709
    //        vc.decodedFrameSource = std::move(src);
    //        vc.loop              = true;
    //        vc.state             = VideoPlaybackState::Playing;
    //        world.AddComponent<VideoComponent>(mp4Ent, std::move(vc));

    //        using namespace DirectX;
    //        LocalTransform lt{};
    //        lt.translation = { -3.0f, 1.5f, 0.0f };   // left of cube + SMPTE plane
    //        lt.rotation    = { 0.0f, 0.0f, 0.0f, 1.0f };
    //        lt.scale       = { 1.0f, 1.0f, 1.0f };
    //        world.AddComponent<LocalTransform>(mp4Ent, lt);

    //        GlobalTransform gt{};
    //        XMStoreFloat4x4(&gt.matrix,
    //            XMMatrixTranslation(lt.translation.x, lt.translation.y, lt.translation.z));
    //        world.AddComponent<GlobalTransform>(mp4Ent, gt);

    //        m_spawnedEntities.push_back(mp4Ent);
    //        LOG_SUCCESS("TestScene: mp4 video plane spawned at (-3, 1.5, 0)");
    //    }
    //    else
    //    {
    //        LOG_INFO("TestScene: no asset/test.mp4 — drop one in to see live "
    //                 "FFmpeg playback. Skipping mp4 plane.");
    //    }
    //}
}

void TestScene::Update(float /*dt*/)
{
    // No per-scene gameplay logic in the default level. ECS systems
    // (script/physics/animation/transform) tick in App and operate on the
    // shared World regardless of which scene is active.
}

void TestScene::Shutdown()
{
    if (!m_ctx || !m_ctx->world)
    {
        LOG_INFO("TestScene: Shutdown (world already gone)");
        return;
    }
    World& world = *m_ctx->world;
    // Release any video GPU resources BEFORE the entities are destroyed —
    // VideoComponent lives in the pool; teardown must walk it while it's
    // still there. (Entity destruction wipes the component which would
    // otherwise leak the decoder + DPB textures.)
    world.ForEach<VideoComponent>(
        [&](Entity /*e*/, VideoComponent& vc)
    {
        Video::ReleaseGPUResources(m_ctx->gfx, vc);
    });

    for (Entity e : m_spawnedEntities)
        if (world.IsAlive(e)) world.DestroyEntity(e);
    m_spawnedEntities.clear();
    LOG_INFO("TestScene: Shutdown");
}
