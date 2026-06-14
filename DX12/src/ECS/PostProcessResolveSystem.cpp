#include "ECS/PostProcessResolveSystem.h"

#include "ECS/ECS.h"
#include "ECS/FrameContext.h"
#include "ECS/HierarchyComponents.h"      // GlobalTransform
#include "ECS/CameraStackSystem.h"        // Camera::FindChannelEntity
#include "ECS/CameraStackComponents.h"    // LiveCameraComponent, Camera::kMainChannel

#include "PostProcess/PostProcessResolve.h"

using namespace DirectX;

void PostProcessResolveSystem::Update(World& world, const FrameContext& ctx)
{
    // Final blended view position: prefer the resolved Live camera (matches what
    // the Renderer will use), fall back to the camera entity's GlobalTransform.
    XMFLOAT3 cameraPos{ 0.0f, 0.0f, 0.0f };
    bool got = false;

    if (Entity ch = Camera::FindChannelEntity(world, Camera::kMainChannel);
        ch != NullEntity)
    {
        if (const auto* live = world.GetComponent<LiveCameraComponent>(ch))
        {
            cameraPos = live->position;
            got = true;
        }
    }
    if (!got && ctx.cameraEntity != 0)
    {
        if (const auto* gt = world.GetComponent<GlobalTransform>(
                static_cast<Entity>(ctx.cameraEntity)))
        {
            cameraPos = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
        }
    }

    // Single main view in this engine; layer mask sees everything.
    PostProcess::ResolveForView(world, cameraPos, 0xFFFFFFFFu, ctx.deltaTime);
}
