#pragma once

// FollowSystem — drives GlobalTransform of entities with Follow* components.
//
// Execution order (see Renderer::Render):
//   AnimationSystem → IK → ChainPhysics → LocalToWorldSystem → SocketSystem
//     → FollowSystem → SkinMatrix
//
// Runs after SocketSystem so FollowSocketComponent can read the fresh
// socket worldTransform cache published this frame; TestScene::Update has
// already run TransformSystem::Propagate earlier in the frame, so
// hierarchy-driven targets are also up to date.
//
// This system does NOT resolve follow-chains (A follows B follows C) in
// arbitrary order in a single pass — for chains, insert a second
// FollowSystem::Update or run Update twice. Current usage (effect → weapon)
// is one-level deep so a single pass suffices.

#include "ECS/ECS.h"

class FollowSystem
{
public:
    void Update(World& world);
};
