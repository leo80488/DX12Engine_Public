#include "Scene/SceneDefaults.h"

#include "ECS/Components.h"
#include "ECS/CameraSystem.h"
#include "ECS/SkyboxComponent.h"

void Scene::SpawnDefaultWorld(World& world)
{
    // Camera — App's "main camera" hint latches onto the first entity carrying
    // a CameraComponent, so just ensure one exists. Pose lives on
    // LocalTransform; FPS-controller state on CameraControllerComponent.
    Entity cam = world.CreateEntity();
    world.SetName(cam, "Main Camera");
    CameraControllerComponent camCtrl{};
    world.AddComponent<CameraComponent>(cam, CameraComponent{});
    world.AddComponent<CameraControllerComponent>(cam, camCtrl);
    world.AddComponent<LocalTransform>(cam, CameraSystem::MakeTransform(camCtrl, { 4.f, 3.f, 5.f }));
    world.AddComponent<GlobalTransform>(cam, GlobalTransform{});

    // Directional light
    Entity light = world.CreateEntity();
    world.SetName(light, "Directional Light");
    LightData ld;
    ld.direction = { 0.447f, -0.894f, 0.224f };
    ld.color     = { 1.f, 0.92f, 0.82f };
    ld.intensity = 1.f;
    ld.type      = LightType::Directional;
    world.AddComponent<LightData>(light, ld);

    // Skybox + IBL environment
    Entity sky = world.CreateEntity();
    world.SetName(sky, "Skybox");
    SkyboxComponent sc;
    sc.irradiancePath    = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Irradiance.itex";
    sc.radiancePath      = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Radiance.itex";
    sc.skyboxPath        = "asset/IBL/autumn_field_puresky/autumn_field_puresky_skybox.itex";
    sc.radianceMipLevels = 7;
    sc.iblStrength       = 1.0f;
    world.AddComponent<SkyboxComponent>(sky, sc);
}
