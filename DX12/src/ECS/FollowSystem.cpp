#include "ECS/FollowSystem.h"
#include "ECS/FollowComponents.h"
#include "ECS/FollowEvents.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "System/EventBus.h"

#include <DirectXMath.h>

using namespace DirectX;

namespace
{
    // Write finalWorld into follower's GlobalTransform, and decompose into
    // LocalTransform so editor gizmos / serializers / child hierarchy stays
    // in sync. Matches SocketSystem's convention exactly.
    inline void ApplyFollow(World& world, Entity follower, FXMMATRIX finalWorld)
    {
        if (GlobalTransform* gt = world.GetComponent<GlobalTransform>(follower))
            XMStoreFloat4x4(&gt->matrix, finalWorld);

        if (LocalTransform* lt = world.GetComponent<LocalTransform>(follower))
        {
            XMVECTOR s, r, t;
            if (XMMatrixDecompose(&s, &r, &t, finalWorld))
            {
                XMStoreFloat3(&lt->translation, t);
                XMStoreFloat4(&lt->rotation,    r);
                XMStoreFloat3(&lt->scale,       s);
            }
        }
    }
}

namespace
{
    // Detect valid→invalid transition, publish once, and clear the handle so
    // the next frame doesn't re-fire. Returns true if caller should continue
    // (i.e. handle is valid); false if the follower should be skipped.
    //
    // The "clear on publish" convention matches the design doc's "Entity
    // Reference 的生命週期管理" guidance: rather than forcing every subscriber
    // to decide how to handle a dead target, we commit to one transition
    // event and then the follower is effectively orphaned until a subscriber
    // (e.g. equipment manager) re-assigns it or deletes it.
    bool ResolveTarget(Entity follower, EntityHandle& target, World& world)
    {
        if (world.IsHandleValid(target)) return true;

        if (target.entity != NullEntity)
        {
            EventBus::Get().Publish(FollowTargetLostEvent{ follower, target });
            target = NullEntityHandle;
        }
        return false;
    }
}

void FollowSystem::Update(World& world)
{
    // ---- FollowEntityComponent: track target's GlobalTransform --------------
    world.ForEach<FollowEntityComponent>([&](Entity follower, FollowEntityComponent& fe)
    {
        if (!ResolveTarget(follower, fe.target, world)) return;

        const GlobalTransform* targetGT = world.GetComponent<GlobalTransform>(fe.target.entity);
        if (!targetGT) return;

        const XMMATRIX offset      = XMLoadFloat4x4(&fe.localOffset);
        const XMMATRIX targetWorld = XMLoadFloat4x4(&targetGT->matrix);
        const XMMATRIX finalWorld  = offset * targetWorld;

        ApplyFollow(world, follower, finalWorld);
    });

    // ---- FollowSocketComponent: read target's SocketComponent cache ----------
    world.ForEach<FollowSocketComponent>([&](Entity follower, FollowSocketComponent& fs)
    {
        if (!ResolveTarget(follower, fs.target, world)) return;

        const SocketComponent* sc = world.GetComponent<SocketComponent>(fs.target.entity);
        if (!sc) return;
        if (fs.socketIndex >= sc->count) return;

        const XMMATRIX offset      = XMLoadFloat4x4(&fs.localOffset);
        const XMMATRIX socketWorld = XMLoadFloat4x4(&sc->sockets[fs.socketIndex].worldTransform);
        const XMMATRIX finalWorld  = offset * socketWorld;

        ApplyFollow(world, follower, finalWorld);
    });
}
