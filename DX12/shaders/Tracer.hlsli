// Tracer.hlsli — shared GPU data layouts for the tracer (cylindrical billboard
// thin-laser) system. Layout must match include/Graphics/TracerSystem.h.

#ifndef TRACER_HLSLI
#define TRACER_HLSLI

// 64 bytes. Mirrors C++ TracerGPU.
struct Tracer
{
    float3 startPos;            // 12
    float  lifetime;            // 4   — seconds remaining; <= 0 = dead
    float3 endPos;              // 12
    float  maxLifetime;         // 4
    float4 color;               // 16  — linear HDR; .a = intensity multiplier
    float  width;               // 4   — beam radius in world units
    float  spawnTime;           // 4   — seconds; absolute, for noise scroll continuity
    uint   noiseTexBindlessIdx; // 4   — 0xFFFFFFFFu = procedural noise
    uint   _pad0;               // 4
};

// 64 bytes. CPU pushes one of these per spawn into the per-frame upload buffer.
// Same shape as Tracer for direct CS copy.
struct TracerSpawn
{
    float3 startPos;
    float  lifetime;
    float3 endPos;
    float  maxLifetime;
    float4 color;
    float  width;
    float  spawnTime;
    uint   noiseTexBindlessIdx;
    uint   _pad0;
};

// 32 bytes. Mirrors C++ TracerSystemParams. Bound at b0 space2 for both
// emit + update CS, and at b2 space0 for the render pass (RenderCB).
struct TracerSystemParams
{
    float deltaTime;     // 4
    uint  poolCapacity;  // 4
    uint  spawnCount;    // 4 — number of valid entries in gSpawns this frame
    float time;          // 4 — global time (sec); used by render PS for scroll
    uint  writeCursor;   // 4 — emit CS computes dst = (writeCursor + threadId) % capacity
    uint  _pad0;         // 4
    uint  _pad1;         // 4
    uint  _pad2;         // 4
};

#endif // TRACER_HLSLI
