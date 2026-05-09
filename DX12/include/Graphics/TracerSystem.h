#pragma once

// TracerSystem — GPU pool for cylindrical-billboard tracer beams (thin lasers,
// gun tracers, far-distance energy shots). Designed for thousands of cheap
// short-lived events; not for thick PBR beams (use BeamSystem for those).
//
// Spawn model: gameplay code calls `Spawn(start, end, color, ...)` from
// anywhere — typically a WeaponFireSystem on bullet impact resolve. The
// tracer is enqueued on a CPU vector; BeginFrame drains it into the GPU
// upload buffer. The render system writes one Tracer per spawn, dispatches
// emit + age compute shaders, then draws the entire pool as instanced quads.
//
// Pool / lifecycle:
//   • Single global StructuredBuffer<Tracer> on DEFAULT heap (SRV + UAV).
//   • Ring-buffer cursor — old tracers get overwritten when new spawns wrap
//     past them. With kMaxTracers = 4096 and typical tracer lifetimes < 0.5s,
//     this is ~8000 spawns/sec before pops; raise the pool if you exceed.

#include "Graphics/GraphicsStruct.h"

#include <vector>
#include <cstdint>
#include <DirectXMath.h>

class IGraphicsDevice;

// ---- GPU-visible tracer record. 64 bytes; mirrors shaders/Tracer.hlsli.
struct alignas(16) TracerGPU
{
    DirectX::XMFLOAT3 startPos;            // 12
    float             lifetime;            // 4   — seconds remaining; <=0 = dead
    DirectX::XMFLOAT3 endPos;              // 12
    float             maxLifetime;         // 4
    DirectX::XMFLOAT4 color;               // 16  — linear HDR; .a = intensity
    float             width;               // 4   — beam radius in metres
    float             spawnTime;           // 4   — global time when spawned
    uint32_t          noiseTexBindlessIdx; // 4   — 0xFFFFFFFFu = procedural
    uint32_t          _pad0;               // 4
};
static_assert(sizeof(TracerGPU) == 64, "TracerGPU layout drift");

// CPU pushes one of these per spawn. Same shape as TracerGPU so the emit CS
// is a straight memcpy with `dst = (writeCursor + threadId) % capacity`.
using TracerSpawnGPU = TracerGPU;

// 32 bytes. Mirrors shaders/Tracer.hlsli TracerSystemParams.
struct alignas(16) TracerSystemParams
{
    float    deltaTime;     // 4
    uint32_t poolCapacity;  // 4
    uint32_t spawnCount;    // 4
    float    time;          // 4 — global seconds (for noise scroll)
    uint32_t writeCursor;   // 4 — cursor at start of this frame
    uint32_t _pad0;         // 4
    uint32_t _pad1;         // 4
    uint32_t _pad2;         // 4
};
static_assert(sizeof(TracerSystemParams) == 32, "TracerSystemParams layout drift");

class TracerSystem
{
public:
    // 4096 tracers × 64 B = 256 KB GPU. Enough headroom for an FPS firefight
    // at 2000 spawns/sec for ~2s before ring-wrap pops.
    static constexpr uint32_t kMaxTracers          = 4096;
    static constexpr uint32_t kMaxSpawnsPerFrame   = 1024;

    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // Public spawn API. Anyone (gameplay, debug, scripts) can call this
    // between frames. Drops on the floor if more than kMaxSpawnsPerFrame
    // requests pile up in one frame.
    //
    //   start/end : world-space endpoints (typically muzzle → impact)
    //   color     : linear HDR; .a is an intensity multiplier (>1 ok)
    //   width     : world-space radius (0.02 ~ 0.2 m typical)
    //   lifetime  : seconds
    //   noiseTexBindless : optional bindless-texture index for the noise
    //                      scroll; pass 0xFFFFFFFFu for cheap procedural
    void Spawn(const DirectX::XMFLOAT3& start,
               const DirectX::XMFLOAT3& end,
               const DirectX::XMFLOAT4& color,
               float                    width,
               float                    lifetime,
               uint32_t                 noiseTexBindless = 0xFFFFFFFFu);

    // Drain the pending CPU queue into the GPU upload buffer; advance the
    // ring cursor; refresh the system CB. Call from Renderer::BeginFrame.
    void BeginFrame(float dt, uint32_t frameIndex, float globalTimeSeconds);

    // Number of spawns emitted this frame (after BeginFrame). Used by
    // TracerSimPass to size the emit dispatch.
    uint32_t GetSpawnCount() const { return m_spawnCountThisFrame; }

    // Descriptor accessors for passes.
    const RHI::GPUBuffer& GetPool()        const { return m_pool; }
    uint64_t              GetPoolSRV()     const { return m_poolSrv; }
    uint64_t              GetPoolUAV()     const { return m_poolUav; }
    const RHI::GPUBuffer& GetSpawnBuffer() const { return m_spawnBuffer; }
    uint64_t              GetSpawnSRV()    const { return m_spawnSrv; }
    const RHI::GPUBuffer& GetSystemCB()    const { return m_systemCB; }

    static constexpr uint32_t GetPoolCapacity() { return kMaxTracers; }

private:
    IGraphicsDevice* m_gfx = nullptr;

    // Main pool (DEFAULT heap, SRV + UAV).
    RHI::GPUBuffer m_pool;
    uint64_t       m_poolSrv = 0;
    uint64_t       m_poolUav = 0;

    // Per-frame spawn upload buffer (UPLOAD heap, SRV).
    RHI::GPUBuffer m_spawnBuffer;
    uint64_t       m_spawnSrv = 0;
    void*          m_spawnMapped = nullptr;

    // Per-frame system CB (UPLOAD heap, root CBV).
    RHI::GPUBuffer m_systemCB;
    void*          m_systemCBMapped = nullptr;

    // CPU spawn queue, flushed at BeginFrame.
    std::vector<TracerSpawnGPU> m_pendingSpawns;

    // GPU pool ring cursor — advances by spawn count each frame.
    uint32_t m_writeCursor = 0;

    // Spawn count actually written this frame (after clamping).
    uint32_t m_spawnCountThisFrame = 0;

    // Global time accumulated by BeginFrame (forwarded into the system CB).
    float m_lastGlobalTime = 0.0f;
};
