#include "PostProcess/EntityVolumeSource.h"

#include "ECS/ECS.h"
#include "ECS/VolumeComponent.h"
#include "ECS/HierarchyComponents.h"  // GlobalTransform

namespace PostProcess
{

void EntityVolumeSource::Gather(const Context& ctx, std::vector<Snapshot>& out)
{
    World* world = ctx.world;
    if (!world) return;

    world->ForEach<ECS::VolumeComponent>(
        [&](Entity e, ECS::VolumeComponent& comp)
    {
        const Volume& src = comp.volume;
        if (!src.enabled)                    return;
        if (!src.override.HasAnyOverride())  return;  // nothing to contribute

        // World-space center: prefer the entity's GlobalTransform. Fall back
        // to the component's own center for orphaned volumes (no transform
        // attached — legitimate for code-constructed entities that skip the
        // hierarchy).
        DirectX::XMFLOAT3 worldCenter = src.center;
        if (const auto* gt = world->GetComponent<GlobalTransform>(e))
        {
            worldCenter.x = gt->matrix._41;
            worldCenter.y = gt->matrix._42;
            worldCenter.z = gt->matrix._43;
        }

        // Copy shape + extents, but use the transform-derived center so the
        // SDF math sees the current world position. Cheap — stack allocated.
        Volume world_v = src;
        world_v.center = worldCenter;

        const float sd = ComputeSignedDistance(world_v, ctx.cameraPos);
        const float w  = ComputeWeightFromDistance(sd, world_v.blendDistance);
        if (w <= 0.0f) return;

        // Point at the live component's override. Lifetime is safe as long
        // as no one mutates the ComponentPool mid-blend (Stack::Execute
        // runs serially within a frame, so that's guaranteed).
        out.push_back(Snapshot{ w, src.priority, &comp.volume.override });
    });
}

} // namespace PostProcess
