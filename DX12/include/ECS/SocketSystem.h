#pragma once

// SocketSystem — publishes per-entity socket world transforms every frame.
//
// For every character that owns a SocketComponent, walks its socket list and
// stores each socket's world transform in-place (Socket::worldTransform).
// This is a read-optimized cache; follower entities consume it via
// FollowSocketComponent + FollowSystem — SocketSystem itself no longer
// knows or cares which entities are attached.
//
// Execution order (see Renderer::Render):
//   AnimationSystem → IK → ChainPhysics → LocalToWorldSystem → SocketSystem
//     → FollowSystem → SkinMatrix
//
// PoseRingBuffer at the point this system runs contains SKIN matrices
// (invBindPose * boneModelMatrix). To recover the bone's model-space
// transform we multiply by bindPose:  modelBone = bindPose * skin.
// World-space transform is then:       worldBone = modelBone * rootGlobalTransform.

#include "ECS/ECS.h"

class PoseRingBuffer;
class SkeletonRegistry;

class SocketSystem
{
public:
    SocketSystem(PoseRingBuffer& poseBuffer, SkeletonRegistry& skeletons)
        : m_poseBuffer(poseBuffer), m_skeletons(skeletons) {}

    void Update(World& world);

private:
    PoseRingBuffer&   m_poseBuffer;
    SkeletonRegistry& m_skeletons;
};
