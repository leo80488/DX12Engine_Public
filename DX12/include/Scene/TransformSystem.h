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
};
