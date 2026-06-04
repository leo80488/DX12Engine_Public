#pragma once

// TransformSystem — propagates LocalTransform → GlobalTransform through the
// scene hierarchy (BFS from roots to leaves) and inherits Visibility flags.
//
// Algorithm:
//   1. Find all root entities (those with no Parent, or Parent == NullEntity)
//      and compute their GlobalTransform = LocalTransform.ToMatrix().
//   2. BFS: for each entity dequeued, visit its Children and compute
//      child.GlobalTransform = child.LocalTransform.ToMatrix() * parent.GlobalTransform
//   3. Propagate VisibilityComponent::inheritedHidden from parent to each child.
//
// Complexity: O(N) where N = total entity count with GlobalTransform.
// Call once per frame before rendering, after all Transform mutations.

class World;

class TransformSystem
{
public:
    static void Propagate(World& world);

    // Re-propagate ONLY the descendants of @p root, assuming @p root's own
    // GlobalTransform is already current. Use this after something overwrites a
    // single entity's GlobalTransform *out of band* (i.e. after the global
    // Propagate has already run) and that entity has children that must follow.
    //
    // The motivating case: PhysicsSystem::ApplyRenderInterpolation overwrites a
    // physics body's GlobalTransform with the render-interpolated pose AFTER the
    // frame's Propagate. Without this, a child entity (e.g. the visible skinned
    // mesh parented under a CharacterController capsule) keeps the stale,
    // non-interpolated world transform the earlier Propagate composed — so it
    // renders one physics-step behind the body the camera follows, which reads
    // as jitter that gets worse the closer the camera is. No-op if @p root has
    // no Children. Does NOT recompute @p root itself.
    static void PropagateSubtree(World& world, unsigned int root);
};
