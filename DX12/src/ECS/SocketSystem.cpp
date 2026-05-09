#include "ECS/SocketSystem.h"
#include "ECS/AnimationComponents.h"
#include "ECS/HierarchyComponents.h"
#include "Graphics/SkinningBuffers.h"
#include "Resource/SkeletonAsset.h"

#include <DirectXMath.h>

using namespace DirectX;

namespace
{
    // modelBone = bindPose * skin   (row-vector: bindPose applied first, then skin
    //                                cancels the invBind that skin embeds — leaving
    //                                the bone's current model-space transform).
    inline XMMATRIX RecoverModelBone(const XMFLOAT4X4& bindPose, const XMFLOAT4X4& skin)
    {
        return XMLoadFloat4x4(&bindPose) * XMLoadFloat4x4(&skin);
    }
}

void SocketSystem::Update(World& world)
{
    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;

        const SkeletonComponent* skel = world.GetComponent<SkeletonComponent>(e);
        if (!skel) continue;
        if (skel->assetIndex == kInvalidAnimHandle) continue;
        // ~0u is the only sentinel for "no pose this frame"; 0 IS a valid byte offset.
        if (skel->poseByteOffset == ~0u) continue;

        const XMFLOAT4X4* skinMatrices = m_poseBuffer.ReadMapped(skel->poseByteOffset);
        if (!skinMatrices) continue;

        const SkeletonAsset& asset = m_skeletons.Get(skel->assetIndex);

        const GlobalTransform* rootGT = world.GetComponent<GlobalTransform>(e);
        const XMMATRIX rootWorld = rootGT
            ? XMLoadFloat4x4(&rootGT->matrix)
            : XMMatrixIdentity();

        // Socket defs live on the entity (not the asset) — iterate whatever
        // the user published via SocketComponent::Add / editor. Each socket
        // caches its world transform in-place so FollowSystem is a pure
        // cache reader (no pose-buffer traversal needed on the follower side).
        SocketComponent* sc = world.GetComponent<SocketComponent>(e);
        if (!sc) continue;

        for (uint32_t i = 0; i < sc->count; ++i)
        {
            SocketComponent::Socket& s = sc->sockets[i];
            if (s.boneIndex >= skel->boneCount) continue;

            const XMMATRIX modelBone = RecoverModelBone(
                asset.bindPose[s.boneIndex], skinMatrices[s.boneIndex]);

            const XMMATRIX offset     = XMLoadFloat4x4(&s.localOffset);
            const XMMATRIX worldMat   = offset * modelBone * rootWorld;
            XMStoreFloat4x4(&s.worldTransform, worldMat);
        }
    }
}
