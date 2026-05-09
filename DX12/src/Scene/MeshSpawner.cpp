#include "Scene/MeshSpawner.h"
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "Resource/ProceduralMesh.h"
#include "Resource/MaterialSerializer.h"
#include "System/Log.h"

static const char* kDefaultMatPath = "asset\\Default_Texture\\Default_mat.imat";

Entity MeshSpawner::Spawn(int meshType, World& world)
{
    if (meshType < 0 || meshType >= static_cast<int>(PrimitiveMeshType::Count))
        return NullEntity;

    static const char* names[] = { "Cube", "Sphere", "Cone" };

    Entity e = world.CreateEntity();
    world.SetName(e, names[meshType]);
    world.AddComponent<LocalTransform>(e, LocalTransform{});
    world.AddComponent<GlobalTransform>(e, GlobalTransform{});
    world.AddComponent<Visibility>(e, Visibility{});
    world.AddComponent<RenderLayer>(e, RenderLayer{});
    world.AddComponent<Children>(e, Children{});

    MeshHandle mh;
    mh.gpuMeshID = static_cast<uint32_t>(meshType);
    world.AddComponent<MeshHandle>(e, mh);

    // Unit bounding box used for mouse picking (half-extents fit all three primitives).
    world.AddComponent<BoundingVolume>(e, BoundingVolume{});

    // Load default material from .imat file.
    MaterialComponent mat{};
    if (Resource::LoadMaterial(kDefaultMatPath, mat))
        mat.SetDirty();
    world.AddComponent<MaterialComponent>(e, std::move(mat));
    world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ kDefaultMatPath });

    LOG_INFO("MeshSpawner: spawned %s (entity=%u)", names[meshType], e);
    return e;
}
