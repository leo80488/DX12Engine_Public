#pragma once

// ParticleSystem — GPU particle pool + per-emitter upload infrastructure.
//
// Owns:
//   • A single global pool of kMaxGlobalParticles slots (GPU, structured buffer).
//   • A per-frame upload buffer of emitter descriptors (position + settings +
//     spawn count). Consumed by ParticleEmitPass on the GPU.
//   • A ring-buffer cursor tracking where the next emitted particle writes
//     into the global pool. Old particles get overwritten when the cursor
//     wraps around — acceptable for MVP; tight pools with high spawn rates
//     will see visible pops, which is the cue to grow the pool or limit
//     spawn rate.
//
// Two-step per-frame flow:
//   CPU:   BeginFrame → collect emitters, fill per-emitter upload slot,
//          compute integer spawn count via accumulator, advance cursor.
//   GPU:   ParticleEmitPass (N threadgroups per emitter) → writes new
//          particles into the pool.
//          ParticleUpdatePass (pool/64 threadgroups) → ages + moves every
//          particle in the pool.
//   GPU:   ParticleRenderPass → instanced quad draw, one instance per pool
//          slot, VS self-discards dead slots.

#include "Graphics/GraphicsStruct.h"
#include "Resource/SystemHandles.h"     // Resource::TextureHandle
#include "ECS/ECS.h"

#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <DirectXMath.h>

class IGraphicsDevice;
class World;
namespace Resource {
    class TextureSystem;
    class ResourceManager;
}

// ---- GPU-visible particle data. 80 bytes, matches Particle.hlsli ParticleData.
struct alignas(16) ParticleGPU
{
    DirectX::XMFLOAT3 position;          // 12
    float             lifetime;          // 4   — seconds remaining; <= 0 = dead
    DirectX::XMFLOAT3 velocity;          // 12
    float             maxLifetime;       // 4   — total life (for 0..1 lerp)
    DirectX::XMFLOAT4 startColor;        // 16
    DirectX::XMFLOAT4 endColor;          // 16
    float             startSize;         // 4
    uint32_t          textureBindlessIdx;// 4   — 0xFFFFFFFFu = no texture
    uint32_t          visualMode;        // 4   — ParticleVisualMode enum cast
    float             _pad0;             // 4
};                                        // 80 bytes
static_assert(sizeof(ParticleGPU) == 80, "ParticleGPU layout drift");

// ---- Per-frame per-emitter upload record. Consumed by ParticleEmit.cs.hlsl.
// Exactly 256 bytes so it fits one CBV-aligned slot.
struct alignas(16) ParticleEmitterGPU
{
    DirectX::XMFLOAT3   position;         // 12 — world position
    float               startLifetime;    // 4
    DirectX::XMFLOAT3   velocityMin;      // 12
    float               startSize;        // 4
    DirectX::XMFLOAT3   velocityMax;      // 12
    uint32_t            spawnCount;       // 4
    DirectX::XMFLOAT4   startColor;       // 16
    DirectX::XMFLOAT4   endColor;         // 16
    DirectX::XMFLOAT3   gravity;          // 12
    uint32_t            writeCursor;      // 4
    uint32_t            randomSeed;       // 4
    uint32_t            shapeType;        // 4 — ParticleShape enum
    uint32_t            visualMode;       // 4 — ParticleVisualMode enum
    uint32_t            textureBindlessIdx;// 4 — 0xFFFFFFFFu = none

    // Shape parameters — interpretation depends on shapeType:
    //   Point:   all unused
    //   Sphere:  p0 = (radius, spawnOnShell, _, _)
    //   Cone:    p0 = (radius, halfAngle, _, _); p1 = (dir.xyz, length)
    //   Box:     p0 = (hx, hy, hz, _)
    //   Circle:  p0 = (radius, _, _, _); p1 = (normal.xyz, _)
    //   Mesh:    none (mesh info lives in meshWorldMatrix + meshDescSlot)
    DirectX::XMFLOAT4   shapeParam0;      // 16
    DirectX::XMFLOAT4   shapeParam1;      // 16
    DirectX::XMFLOAT4   shapeParam2;      // 16
    DirectX::XMFLOAT4   shapeParam3;      // 16

    DirectX::XMFLOAT4X4 meshWorldMatrix;  // 64 — used only for Mesh shape

    uint32_t            meshDescSlot;     // 4 — index into MeshDescriptor bindless SB
    uint32_t            meshIndexCount;   // 4
    uint32_t            _padA;            // 4
    uint32_t            _padB;            // 4
};                                         // 256 bytes total
static_assert(sizeof(ParticleEmitterGPU) == 256, "Emitter CB must be exactly 256B");

// ---- Per-frame global state CB consumed by update / render passes.
struct alignas(16) ParticleSystemCB
{
    float             deltaTime;      // 4
    uint32_t          particleCount;  // 4 — total pool capacity
    uint32_t          frameIndex;     // 4
    float             _pad0;          // 4
    DirectX::XMFLOAT3 globalGravity;  // 12 — sampled from the FIRST emitter
    float             _pad1;          // 4
};
static_assert(sizeof(ParticleSystemCB) == 32, "ParticleSystemCB layout drift");

class ParticleSystem
{
public:
    // Total particles in flight across the whole world. 10k * 64B = 640 KB.
    // If you raise this past ~64k you'll want to revisit the ring-buffer
    // write scheme (currently one cursor shared by all emitters).
    static constexpr uint32_t kMaxGlobalParticles = 10000;

    // Upper bound on emitters per frame. Per-emitter upload struct is 112 B.
    static constexpr uint32_t kMaxEmittersPerFrame = 256;

    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // Wire resource systems used for texture-path → bindless-idx resolution.
    // Optional — if unset, emitter textures are ignored.
    void SetResourceSystems(Resource::TextureSystem* texSys,
                            Resource::ResourceManager* resMgr)
    {
        m_texSys = texSys;
        m_resMgr = resMgr;
    }

    // Release the per-entity texture handle when an emitter's owning entity
    // is destroyed. Fan-in via Renderer's entity-destroy listener.
    void OnEntityDestroyed(IGraphicsDevice& gfx, Entity e);

    // Called each frame from Renderer::BeginFrame.
    //   • Iterates ParticleEmitterComponent pool entities.
    //   • Advances per-emitter spawn accumulator, computes integer spawn count.
    //   • Writes ParticleEmitterGPU into mapped upload buffer.
    //   • Updates the ring cursor.
    //   • For mesh-shape emitters, records a deferred mesh resolution request
    //     (Renderer patches meshDescSlot + meshWorldMatrix after this call).
    void CollectEmitters(World& world, float dt, uint32_t frameIndex);

    // Per-emitter pending mesh reference — filled by CollectEmitters,
    // consumed by Renderer::ResolveParticleMeshEmitters right after.
    struct PendingMeshResolve
    {
        uint32_t emitterSlotIndex;  // index into upload buffer
        Entity   sourceEntity;      // mesh source entity
    };
    const std::vector<PendingMeshResolve>& GetPendingMeshResolves() const
    { return m_pendingMeshResolves; }

    // Patches the upload slot at `slotIndex` with mesh descriptor slot +
    // world matrix. Called by Renderer after CollectEmitters.
    void PatchMeshEmitter(uint32_t slotIndex,
                          uint32_t meshDescSlot,
                          uint32_t meshIndexCount,
                          const DirectX::XMFLOAT4X4& worldMatrix);

    // Number of emitter slots written this frame (for dispatch count).
    uint32_t GetEmitterCount()  const { return m_emitterCountThisFrame; }

    // Spawn count for emitter slot `i` (0..GetEmitterCount-1). Used by emit
    // dispatch to size the threadgroup count (ceil(spawnCount/64)).
    uint32_t GetEmitterSpawnCount(uint32_t i) const
    { return i < m_emitterSpawnCounts.size() ? m_emitterSpawnCounts[i] : 0; }

    // Byte size of one emitter upload slot. 256-byte aligned so the emit
    // compute shader can bind CBVs at `emitterIdx * kEmitterSlotStride`.
    static constexpr uint64_t kEmitterSlotStride = 256;

    // Descriptor accessors used by passes.
    const RHI::GPUBuffer& GetParticlePool()    const { return m_particlePool; }
    const RHI::GPUBuffer& GetEmitterBuffer()   const { return m_emitterBuffer; }
    uint64_t              GetPoolSRVHandle()   const { return m_poolSrvHandle; }
    uint64_t              GetPoolUAVHandle()   const { return m_poolUavHandle; }

    // Per-frame globals CB (delta time, frame index, global gravity).
    const RHI::GPUBuffer& GetSystemCB()        const { return m_systemCB; }
    void                  UpdateSystemCB(float dt, uint32_t frameIndex);

    // Total pool slot count (convenience).
    static constexpr uint32_t GetPoolCapacity() { return kMaxGlobalParticles; }

    // Accessors for render pass (blend mode and texture are per-emitter;
    // for MVP we draw one instanced quad per pool slot with a per-particle
    // stream readback, so these live on the emitter side of the shader
    // and we don't need per-frame texture arrays yet).

private:
    IGraphicsDevice* m_gfx = nullptr;

    // Main pool (GPU DEFAULT heap, SRV + UAV).
    RHI::GPUBuffer   m_particlePool;
    uint64_t         m_poolSrvHandle = 0;
    uint64_t         m_poolUavHandle = 0;

    // Emitter upload buffer (UPLOAD heap, root CBV per dispatch).
    // Stride: kEmitterSlotStride (256) so each emitter's slot is 256B-aligned
    // for D3D12 root CBV binding requirements. ParticleEmitterGPU (~112B) is
    // written at the start of each slot; remaining bytes are zeroed padding.
    RHI::GPUBuffer   m_emitterBuffer;
    void*            m_emitterMapped = nullptr;

    // Per-frame globals CB (UPLOAD heap, root CBV).
    RHI::GPUBuffer   m_systemCB;
    void*            m_systemCBMapped = nullptr;

    // Ring cursor into m_particlePool — advanced by CollectEmitters.
    uint32_t         m_writeCursor = 0;

    // Per-emitter data captured this frame (parallels the upload buffer):
    //   m_emitterSpawnCounts[i] = spawn count for emitter slot i.
    //   m_globalGravity         = gravity from emitter 0 (or zero if none).
    uint32_t                         m_emitterCountThisFrame = 0;
    std::vector<uint32_t>            m_emitterSpawnCounts;
    DirectX::XMFLOAT3                m_globalGravity = { 0.0f, 0.0f, 0.0f };

    // Deferred mesh-shape resolution requests (Renderer patches after call).
    std::vector<PendingMeshResolve>  m_pendingMeshResolves;

    // Texture resolution dependencies (optional — null = no texture support).
    Resource::TextureSystem*   m_texSys = nullptr;
    Resource::ResourceManager* m_resMgr = nullptr;

    // Per-entity texture cache (parallel to MaterialSystem's m_matTexCache).
    // Keeps the acquired TextureHandle alive until the path changes or the
    // emitter is destroyed. On path change we Release the old handle so the
    // texture system can evict the image.
    struct EmitterTexCache
    {
        std::string             path;
        Resource::TextureHandle handle = Resource::kInvalidTextureHandle;
    };
    std::unordered_map<Entity, EmitterTexCache> m_emitterTexCache;
};
