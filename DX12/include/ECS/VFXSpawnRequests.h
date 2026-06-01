#pragma once

// VFXSpawnRequests — "Lane B" mailbox components for the unified VFX spawner.
//
// VFXSpawnSystem (TickPhase::BoneAttachment) can directly spawn the
// component-driven effect types (Particle / Trail / Beam) because they are
// pure ECS. The remaining types need a Renderer-owned GPU pool or asset
// library to instantiate:
//   - Tracer     → TracerSystem::Spawn          (GPU ring, needs IGraphicsDevice)
//   - Decal      → DecalSpawner + DecalMaterialLibrary (Renderer-owned)
//   - Afterimage → AfterimageSystem::Spawn       (fixed snapshot pool; snapshots
//                                                 the spawner's skinned sub-meshes)
//   - Mesh       → MeshLibrary load              (path → GPU mesh; Renderer-owned)
//
// To keep the ECS consumer free of any IGraphicsDevice / Renderer handle (the
// architecture boundary in memory/project_renderer_architecture), VFXSpawnSystem
// only DEPOSITS fully-resolved request structs into these mailbox components on
// the spawner entity. Renderer::BeginFrame (Render phase, which runs AFTER
// BoneAttachment in the same frame — see App.cpp phase order) drains them and
// issues the actual Spawn() calls, then clears the vectors. This is the same
// producer→Renderer pattern ParticleSystem already uses to pick up
// ParticleEmitterComponent.
//
// Lifecycle: like every Pending* mailbox, the producer pushes back and the
// consumer (Renderer) clears. Drained same-frame, so no cross-frame staleness.

#include "ECS/ECS.h"

#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

// ---- Tracer (TracerSystem::Spawn) -----------------------------------------
struct PendingTracerSpawns
{
    struct Req {
        DirectX::XMFLOAT3 start;
        DirectX::XMFLOAT3 end;
        DirectX::XMFLOAT4 color;             // linear HDR; .a = intensity
        float             width;
        float             lifetime;
        uint32_t          noiseTexBindless;  // 0xFFFFFFFF = procedural
    };
    std::vector<Req> reqs;
};

// ---- Decal (DecalSpawner::SpawnAtSurface, material via DecalMaterialLibrary)
struct PendingDecalSpawns
{
    struct Req {
        std::string       materialName;      // resolved via DecalMaterialLibrary::Find
        DirectX::XMFLOAT3 position;          // world hit point (baked from spawn pose)
        DirectX::XMFLOAT3 normal;            // surface normal the decal projects onto
        DirectX::XMFLOAT2 size;              // decal width/height (world units)
        float             depth;             // projection-box thickness
        float             lifetime;          // seconds; < 0 = static
        float             fadeOutDuration;
        float             rollZ;             // rotation about projection axis (rad)
        DirectX::XMFLOAT4 tintOverride;      // w>0 replaces asset tint
    };
    std::vector<Req> reqs;
};

// ---- Afterimage (AfterimageSystem::Spawn on the spawner's skinned children)
struct PendingAfterimageSpawns
{
    struct Req {
        EntityHandle      target;            // spawner character; Renderer fans out via SkeletonRef
        float             lifetime;
        DirectX::XMFLOAT4 color;             // HDR rgb tint (a ignored)
    };
    std::vector<Req> reqs;
};

// ---- Mesh VFX (runtime path → GPU mesh; Renderer patches the created entity)
// The entity is already created by VFXSpawnSystem with transform / attach /
// lifetime; Renderer only resolves the mesh path to a MeshHandle/MeshLibRef and
// patches it (plus the BoundingVolume) onto the entity.
struct PendingMeshVFXSpawns
{
    struct Req {
        EntityHandle      entity;            // pre-created renderable awaiting a GPU mesh
        std::string       meshPath;          // .imsh
        std::string       materialPath;      // .imat (may be empty → default material)
        bool              castShadow;
    };
    std::vector<Req> reqs;
};
