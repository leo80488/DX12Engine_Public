#pragma once

// AfterimageSystem — pool of skinned-mesh pose snapshots for "dodge ghost"
// VFX. Captures the post-skinning vertex output by GPU-copying a slice of
// SkinnedVertexRing into a long-lived snapshot pool buffer. Each live slot
// is drawn as a regular static mesh through the engine's normal Transparent
// + custom-PS path (Afterimage_Ghost.ps.hlsl, additive blend).
//
// Slot model:
//   - kMaxSnapshots fixed slots; each owns a dedicated 32K-vertex slice in
//     a single shared pos+nrm pool buffer (DEFAULT heap, UAV+SRV).
//   - Each slot has a stable MeshDescriptor index in the bindless heap,
//     updated on capture to point at its pool offsets + the source mesh's
//     UV / index / tangent streams.
//   - Each slot owns a MaterialComponent (one per slot — different colors
//     and alphas need separate material slots in the per-frame upload).
//
// Capture flow per frame:
//   1. Gameplay calls Spawn(skinnedEntity, lifetime, color); request queued
//      on m_pendingSpawns.
//   2. Renderer::BeginFrame ticks lifetimes, frees dead slots, then drains
//      pending spawns: looks up the entity's MeshSkinnedComponent +
//      SkinningOutputComponent, allocates a free slot, snapshots the world
//      matrix and static-stream bindless indices, and pushes an
//      AfterimageCaptureJob describing the source/dest byte offsets.
//   3. AfterimageCapturePass dispatches the copy CS for every queued job
//      AFTER SkinningPass has written its output and emitted UAV barriers.
//   4. Renderer's candidate emitter pushes one DrawCandidate per live slot
//      into the standard sort/batch pipeline.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/GraphicsStruct.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"

#include <array>
#include <cstdint>
#include <vector>
#include <DirectXMath.h>

class IGraphicsDevice;
class MeshDescriptorHeap;
class SkinnedVertexRing;
class World;

class AfterimageSystem
{
public:
    // A character with many sub-mesh entities (body, hair, clothes, …) plus a
    // multi-snapshot burst can need 100+ live slots, so the pool errs on
    // generous. Total: 128 × 16K verts × 12 B × 2 buffers = 48 MB.
    static constexpr uint32_t kMaxSnapshots = 128;
    static constexpr uint32_t kVertsPerSlot = 16u * 1024u; // 16K verts per slot
    static constexpr uint32_t kSlotPosBytes = kVertsPerSlot * 12u; // float3
    static constexpr uint32_t kSlotNrmBytes = kVertsPerSlot * 12u;

    // Per-snapshot CPU+GPU state. Kept POD-ish so iteration in the renderer's
    // candidate emitter is cache friendly.
    struct Slot
    {
        bool                 alive          = false;
        float                lifetime       = 0.f;   // seconds remaining
        float                maxLifetime    = 0.f;   // seconds at spawn
        DirectX::XMFLOAT4    color          = { 1.f, 1.f, 1.f, 1.f }; // HDR; rgb tint, a unused (decay computed each frame)
        DirectX::XMFLOAT4X4  worldMatrix    = {};    // capture-time world transform (row-major, row-vector convention)
        DirectX::XMFLOAT3    worldCenter    = { 0.f, 0.f, 0.f }; // AABB center transformed for transparent depth sort
        uint32_t             meshDescSlot   = 0xFFFFFFFFu; // stable bindless MeshDescriptor index
        uint32_t             vertexCount    = 0;
        uint32_t             indexCount     = 0;
        // Owned per-slot material — Renderer's DrawCandidate.mc points at this.
        MaterialComponent    material;
    };

    // Per-frame copy descriptor, consumed by AfterimageCapturePass.
    struct CaptureJob
    {
        uint64_t srcPosSrvHandle;
        uint64_t srcNrmSrvHandle;
        uint32_t srcPosElementBase; // (outPosByteOffset / 12) at capture-time
        uint32_t srcNrmElementBase;
        uint32_t dstPosElementBase; // (slot * kVertsPerSlot)
        uint32_t dstNrmElementBase;
        uint32_t vertexCount;
    };

    void Init(IGraphicsDevice& gfx, MeshDescriptorHeap& heap);
    void Shutdown(IGraphicsDevice& gfx);

    // Queue a snapshot of the named skinned entity. Resolved during BeginFrame.
    // - skinnedEntity must have MeshSkinnedComponent + SkinningOutputComponent
    //   when BeginFrame runs (otherwise the request is silently dropped).
    // - lifetime in seconds; alpha decays linearly from 1.0 → 0.0 over lifetime.
    // - color: HDR RGB tint; .a is ignored (per-snapshot fade is automatic).
    void Spawn(Entity skinnedEntity, float lifetime, const DirectX::XMFLOAT4& color);

    // Per-frame tick. Decays lifetimes, frees expired slots, drains
    // m_pendingSpawns into m_slots, and rebuilds m_captureJobs for the
    // AfterimageCapturePass to consume this frame.
    // Must run AFTER SkinnedMeshSubsystem::BuildSkinJobs so SkinningOutput
    // offsets reflect this frame's writes.
    void BeginFrame(World&              world,
                    MeshDescriptorHeap& heap,
                    SkinnedVertexRing&  vertRing,
                    float               dt);

    // Wipe all live snapshots — called from Renderer::OnWorldClear.
    void OnWorldClear();

    // Drop any pending spawn requests targeting `e`. Does NOT clear live
    // snapshots that were already captured (they're decoupled from the
    // source entity after capture — that's the whole point of snapshots).
    void OnEntityDestroyed(Entity e);

    // Accessors used by Renderer + AfterimageCapturePass.
    const std::array<Slot, kMaxSnapshots>& GetSlots() const { return m_slots; }
    std::array<Slot, kMaxSnapshots>&       GetSlotsMutable() { return m_slots; }
    const std::vector<CaptureJob>&         GetCaptureJobs() const { return m_captureJobs; }

    // Pool buffer accessors for AfterimageCapturePass + memory barriers.
    const RHI::GPUBuffer& GetPoolPosBuffer() const { return m_poolPos; }
    const RHI::GPUBuffer& GetPoolNrmBuffer() const { return m_poolNrm; }
    uint64_t              GetPoolPosUAV()    const { return m_poolPosUAV; }
    uint64_t              GetPoolNrmUAV()    const { return m_poolNrmUAV; }
    uint32_t              GetPoolPosBindless() const { return m_poolPosBindless; }
    uint32_t              GetPoolNrmBindless() const { return m_poolNrmBindless; }

    // Configurable fresnel / base alpha / rim intensity. Defaults are sane;
    // tweak from the editor or per-call site if needed.
    void SetGhostParams(float fresnelPower, float baseAlpha, float rimIntensity);

    bool IsInitialised() const { return m_initialised; }

private:
    uint32_t FindFreeSlot();
    void     ConfigureSlotMaterial(Slot& slot);

    std::array<Slot, kMaxSnapshots> m_slots{};

    struct PendingSpawn
    {
        Entity              entity;
        float               lifetime;
        DirectX::XMFLOAT4   color;
    };
    std::vector<PendingSpawn> m_pendingSpawns;
    std::vector<CaptureJob>   m_captureJobs;

    RHI::GPUBuffer m_poolPos;
    RHI::GPUBuffer m_poolNrm;
    uint64_t       m_poolPosUAV      = 0;
    uint64_t       m_poolNrmUAV      = 0;
    uint64_t       m_poolPosSRV      = 0;
    uint64_t       m_poolNrmSRV      = 0;
    uint32_t       m_poolPosBindless = 0xFFFFFFFFu;
    uint32_t       m_poolNrmBindless = 0xFFFFFFFFu;

    // Ghost-shader knobs — packed into customParams["GhostParams"] each frame.
    float m_fresnelPower = 3.0f;
    float m_baseAlpha    = 0.08f;
    float m_rimIntensity = 2.0f;

    bool m_initialised = false;
};
