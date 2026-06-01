#pragma once

// Flat-binding follow components.
//
// Design doc §"Flat Entity vs Scene Graph Hierarchy":
//   dynamic attachment (a fire emitter following a sword, a trail following a
//   projectile) is expressed as a component reference, not as a parent-child
//   hierarchy. FollowSystem runs after SocketSystem and TransformSystem::Propagate
//   so that by the time it reads target.GlobalTransform that transform has
//   already been updated this frame.
//
// Lifetime safety: target is an EntityHandle (not a raw Entity) so destroying
// the target does not silently retarget the follower at a recycled slot.
// FollowSystem drops entries whose handle fails IsHandleValid.

#include "ECS/ECS.h"

#include <DirectXMath.h>

// ---------------------------------------------------------------------------
// FollowEntityComponent — follower's world transform = localOffset * target.GlobalTransform.
// Placed on the FOLLOWER entity (effect / trail / camera rig).
// ---------------------------------------------------------------------------
struct FollowEntityComponent
{
    EntityHandle        target;
    DirectX::XMFLOAT4X4 localOffset;
    // Editor authoring representation for rotation (same purpose as
    // SocketComponent::Socket::rotationEulerDeg — avoids quat↔euler jitter
    // when dragging the inspector). Runtime uses localOffset matrix only.
    DirectX::XMFLOAT3   rotationEulerDeg = { 0.f, 0.f, 0.f };

    FollowEntityComponent() { DirectX::XMStoreFloat4x4(&localOffset, DirectX::XMMatrixIdentity()); }
};

// ---------------------------------------------------------------------------
// FollowSocketComponent — follower follows a named socket published by target.
//
// Target must own SocketComponent (see AnimationComponents.h). socketIndex is
// an index into that target's SocketComponent::sockets array — editor picks
// by socket name, gameplay can look up an index by name at equip time. The
// follower reads SocketSystem's per-frame worldTransform cache directly, so
// this is a pure cache dependency (no skeleton / pose-buffer access needed
// on the follower side).
//
// Canonical way to attach weapons, shields, and VFX to a character:
//   equip   = world.AddComponent<FollowSocketComponent>(item, { target, sIdx, offset })
//   unequip = world.RemoveComponent<FollowSocketComponent>(item)
// The character has no record of who follows it — query FollowSocket's pool
// for reverse lookup.
// ---------------------------------------------------------------------------
struct FollowSocketComponent
{
    EntityHandle        target;             // the character that owns SocketComponent
    uint32_t            socketIndex = 0;    // index into target SocketComponent::sockets
    DirectX::XMFLOAT4X4 localOffset;
    DirectX::XMFLOAT3   rotationEulerDeg = { 0.f, 0.f, 0.f }; // authoring repr (see FollowEntityComponent)

    FollowSocketComponent() { DirectX::XMStoreFloat4x4(&localOffset, DirectX::XMMatrixIdentity()); }
};
