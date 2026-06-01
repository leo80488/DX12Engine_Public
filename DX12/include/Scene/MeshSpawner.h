#pragma once

// MeshSpawner — single-responsibility helper for creating procedural-mesh entities.
//
// Knows only about ECS (World, Entity, Transform, MeshHandle) and PrimitiveMeshType.
// Has no dependency on the Renderer or any GPU resource.

#include "ECS/ECS.h"

class World;

class MeshSpawner
{
public:
    // Creates an ECS entity at the world origin with a default Transform and
    // a MeshHandle pointing to the pre-uploaded GPU primitive for meshType.
    // meshType: 0=Cube, 1=Sphere, 2=Cone, 3=Plane, 4=Torus  (see PrimitiveMeshType)
    // Returns the created entity, or NullEntity if meshType is out of range.
    static Entity Spawn(int meshType, World& world);
};
