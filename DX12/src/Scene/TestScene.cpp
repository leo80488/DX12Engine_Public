#include "Scene/TestScene.h"
#include "ECS/Components.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/TerrainComponent.h"
#include "ECS/HierarchyComponents.h"
#include "Scene/MeshSpawner.h"
#include "System/Log.h"

void TestScene::Init(SceneContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->world)
    {
        LOG_ERROR("TestScene: Init received null SceneContext / World");
        return;
    }
    LOG_INFO("TestScene: Init");

    World& world = *m_ctx->world;

    // Camera entity — App keeps a "main camera" hint pointing at the first
    // entity it finds carrying CameraComponent, so we just need to ensure
    // there is one. Subsequent scenes can override by spawning their own.
    Entity cam = world.CreateEntity();
    world.SetName(cam, "Main Camera");
    world.AddComponent<CameraComponent>(cam, CameraComponent{});
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

    // Mesh-shader terrain demo — heightmap-displaced tile with 4 PBR layers
    // auto-blended by altitude + slope (no splatmap authored yet, the PS
    // synthesises weights from world Y and surface normal).
    //
    // Sized so the default-camera position (eye=(4,3,5), look-at origin)
    // lands inside the tile and the heightScale clears the camera height.
    {
        Entity terrainEnt = world.CreateEntity();
        world.SetName(terrainEnt, "Terrain");
		float heightScale = 4096.0f; // dramatic relief so peaks/valleys are obvious
        TerrainComponent tc;
        tc.heightmapPath = "asset/EngineResource/Terrain/HeightMap.itex";
        tc.worldCenter   = { 0.0f, -heightScale/2, 0.0f };  // pivot pinned at origin
        tc.worldSize     = 4096.0f;
        tc.heightScale   = heightScale;     // dramatic relief so peaks/valleys are obvious

        const char* kTerrainDir = "asset/EngineResource/Terrain/";

        // Tile spans world Y in [worldCenter.y, worldCenter.y + heightScale]
        // = [-2048, 2048]. The heightmap data clusters above ~20% so the
        // effective visible range starts around -1228 m. The four layers
        // below carve that range up using world-meter heights + slopes —
        // both directly intuitive in the inspector.

        // Layer 0 — grass+rock (mid-high altitude, gentle slope)
        tc.layers[0].albedoPath    = std::string(kTerrainDir) + "aerial_grass_rock_diff_1k.itex";
        tc.layers[0].normalPath    = std::string(kTerrainDir) + "aerial_grass_rock_nor_dx_1k.itex";
        tc.layers[0].armPath       = std::string(kTerrainDir) + "aerial_grass_rock_arm_1k.itex";
        tc.layers[0].dispPath      = std::string(kTerrainDir) + "aerial_grass_rock_disp_1k.itex";
        tc.layers[0].tilingScale   = 0.50f;
        tc.layers[0].minHeight     =  -200.0f;
        tc.layers[0].maxHeight     =  1500.0f;
        tc.layers[0].fadeHeight    =   600.0f;
        tc.layers[0].minSlopeDeg   =  0.0f;
        tc.layers[0].maxSlopeDeg   = 40.0f;
        tc.layers[0].fadeSlopeDeg  =  8.0f;

        // Layer 1 — pure rocks (cliffs / peaks). Anywhere with a steep slope
        // OR very high altitude.
        tc.layers[1].albedoPath    = std::string(kTerrainDir) + "aerial_rocks_02_diff_1k.itex";
        tc.layers[1].normalPath    = std::string(kTerrainDir) + "aerial_rocks_02_nor_dx_1k.itex";
        tc.layers[1].armPath       = std::string(kTerrainDir) + "aerial_rocks_02_arm_1k.itex";
        tc.layers[1].dispPath      = std::string(kTerrainDir) + "aerial_rocks_02_disp_1k.itex";
        tc.layers[1].tilingScale   = 0.50f;
        tc.layers[1].minHeight     =   900.0f;
        tc.layers[1].maxHeight     = 10000.0f;   // open top — let it run to the peaks
        tc.layers[1].fadeHeight    =   600.0f;
        tc.layers[1].minSlopeDeg   = 30.0f;
        tc.layers[1].maxSlopeDeg   = 90.0f;
        tc.layers[1].fadeSlopeDeg  = 12.0f;

        // Layer 2 — river pebbles (low altitude, very gentle slope)
        tc.layers[2].albedoPath    = std::string(kTerrainDir) + "ganges_river_pebbles_diff_1k.itex";
        tc.layers[2].normalPath    = std::string(kTerrainDir) + "ganges_river_pebbles_nor_dx_1k.itex";
        tc.layers[2].armPath       = std::string(kTerrainDir) + "ganges_river_pebbles_arm_1k.itex";
        tc.layers[2].dispPath      = std::string(kTerrainDir) + "ganges_river_pebbles_disp_1k.itex";
        tc.layers[2].tilingScale   = 0.50f;
        tc.layers[2].minHeight     = -10000.0f;  // open bottom — riverbeds, valleys
        tc.layers[2].maxHeight     =  -300.0f;
        tc.layers[2].fadeHeight    =   600.0f;
        tc.layers[2].minSlopeDeg   =  0.0f;
        tc.layers[2].maxSlopeDeg   = 25.0f;
        tc.layers[2].fadeSlopeDeg  =  8.0f;

        // Layer 3 — stone pathway (mid altitude, gentle slope)
        tc.layers[3].albedoPath    = std::string(kTerrainDir) + "stone_pathway_diff_1k.itex";
        tc.layers[3].normalPath    = std::string(kTerrainDir) + "stone_pathway_nor_dx_1k.itex";
        tc.layers[3].armPath       = std::string(kTerrainDir) + "stone_pathway_arm_1k.itex";
        tc.layers[3].dispPath      = std::string(kTerrainDir) + "stone_pathway_disp_1k.itex";
        tc.layers[3].tilingScale   = 0.50f;
        tc.layers[3].minHeight     = -1300.0f;
        tc.layers[3].maxHeight     =  1000.0f;
        tc.layers[3].fadeHeight    =   500.0f;
        tc.layers[3].minSlopeDeg   =  0.0f;
        tc.layers[3].maxSlopeDeg   = 35.0f;
        tc.layers[3].fadeSlopeDeg  =  8.0f;

        world.AddComponent(terrainEnt, tc);
        m_spawnedEntities.push_back(terrainEnt);
    }


    // Default cube at origin — MeshSpawner returns the new entity in the
    // currently-active world; we don't track it here because gameplay tests
    // typically want it to persist through scene-pop (this is the demo cube).
    MeshSpawner::Spawn(0, world);
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
    for (Entity e : m_spawnedEntities)
        if (world.IsAlive(e)) world.DestroyEntity(e);
    m_spawnedEntities.clear();
    LOG_INFO("TestScene: Shutdown");
}
