#pragma once

// TrailSystem — GPU trail ring buffers + per-frame append infrastructure.
//
// Layout: one DEFAULT-heap structured buffer of TrailSegmentGPU sized
// kMaxTrails * kMaxSegmentsPerTrail, so trail `t`'s segments occupy
// slots [t*kMaxSegmentsPerTrail, (t+1)*kMaxSegmentsPerTrail). A second
// header buffer holds per-trail metadata (ring cursor + count + width +
// colours). Both are GPU-writable so TrailUpdatePass can append new
// segments and advance the ring cursor atomically.
//
// Per-frame flow:
//   CPU:  CollectTrails → walk TrailComponent pool, allocate slots,
//         filter by minSampleDistance, write append requests into the
//         UPLOAD "requests" buffer.
//   GPU:  TrailUpdatePass compute → one thread per request. Appends a
//         TrailSegmentGPU into the segment pool at its trail's head,
//         advances the header's cursor, clamps count at kMaxSegmentsPerTrail.
//   GPU:  TrailRenderPass → per-trail draw, VS extrudes segment pairs into
//         a camera-facing ribbon quad.

#include "Graphics/GraphicsStruct.h"
#include "ECS/ECS.h"

#include <vector>
#include <cstdint>
#include <DirectXMath.h>

class IGraphicsDevice;
class World;

// ---- One ribbon segment. 16 bytes.
struct alignas(16) TrailSegmentGPU
{
    DirectX::XMFLOAT3 position;   // 12 — world-space sample
    float             age;        // 4  — seconds since spawn (starts at 0, grows each frame)
};
static_assert(sizeof(TrailSegmentGPU) == 16, "TrailSegmentGPU drift");

// ---- Per-trail metadata. 80 bytes.
struct alignas(16) TrailHeaderGPU
{
    uint32_t          head;        // 4 — next write slot within the trail's ring
    uint32_t          count;       // 4 — current segment count (capped at kMaxSegmentsPerTrail)
    float             width;       // 4
    float             maxAge;      // 4
    DirectX::XMFLOAT4 startColor;  // 16
    DirectX::XMFLOAT4 endColor;    // 16
    // Reserved so future fields (texture handle, UV mode, etc.) can land
    // without disturbing the binding layout.
    uint32_t          _reserved[8]; // 32
};
static_assert(sizeof(TrailHeaderGPU) == 80, "TrailHeaderGPU drift");

// ---- One append request from CPU. 32 bytes (256B-aligned CB slot on upload).
struct alignas(16) TrailAppendRequestGPU
{
    DirectX::XMFLOAT3 position;    // 12 — new segment world position
    float             dt;          // 4  — seconds of per-existing-segment aging this frame
    uint32_t          trailSlot;   // 4  — target trail index in segment pool
    float             width;       // 4
    float             maxAge;      // 4
    float             _pad0;       // 4
    DirectX::XMFLOAT4 startColor;  // 16 — header is refreshed every frame
    DirectX::XMFLOAT4 endColor;    // 16
};
static_assert(sizeof(TrailAppendRequestGPU) == 64, "TrailAppendRequestGPU drift");

// ---- Per-frame globals (cbuffer). 16 bytes.
struct alignas(16) TrailSystemCB
{
    float    deltaTime;     // 4
    uint32_t requestCount;  // 4 — number of append requests this frame
    uint32_t maxSegments;   // 4 — kMaxSegmentsPerTrail, for wrap math in shader
    uint32_t maxTrails;     // 4
};
static_assert(sizeof(TrailSystemCB) == 16, "TrailSystemCB drift");

class TrailSystem
{
public:
    static constexpr uint32_t kMaxTrails           = 64;
    static constexpr uint32_t kMaxSegmentsPerTrail = 64;
    // Per-append-request CB slot stride (256B aligned for root CBV).
    static constexpr uint64_t kRequestSlotStride   = 256;

    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // CPU-side per-frame collection. Walks TrailComponent pool, assigns
    // trail slots on first sight, filters by minSampleDistance, writes
    // append records into the upload buffer. After this call,
    // GetRequestCount() is the number of valid entries in the request buffer.
    void CollectTrails(World& world, float dt);

    // Number of append requests populated this frame.
    uint32_t GetRequestCount() const { return m_requestCountThisFrame; }

    // Max trails currently leased — used by the render pass to size its
    // per-trail draw loop. Not strictly necessary (renderer could walk the
    // entire header buffer), but avoids drawing unused slots.
    uint32_t GetMaxActiveSlot() const { return m_maxActiveSlot; }

    // Buffer accessors for passes.
    const RHI::GPUBuffer& GetSegmentBuffer() const { return m_segmentBuffer; }
    const RHI::GPUBuffer& GetHeaderBuffer()  const { return m_headerBuffer; }
    const RHI::GPUBuffer& GetRequestBuffer() const { return m_requestBuffer; }
    const RHI::GPUBuffer& GetSystemCB()      const { return m_systemCB; }

    uint64_t GetSegmentSRVHandle() const { return m_segSrv; }
    uint64_t GetSegmentUAVHandle() const { return m_segUav; }
    uint64_t GetHeaderSRVHandle()  const { return m_hdrSrv; }
    uint64_t GetHeaderUAVHandle()  const { return m_hdrUav; }

    void UpdateSystemCB(float dt);

private:
    // Lazy slot allocation for a new TrailComponent. Returns ~0u if the
    // pool is exhausted.
    uint32_t AllocateSlot();
    void     ReleaseSlot(uint32_t slot);

    IGraphicsDevice* m_gfx = nullptr;

    // Main pools (DEFAULT heap, SR+UAV).
    RHI::GPUBuffer m_segmentBuffer;
    RHI::GPUBuffer m_headerBuffer;
    uint64_t       m_segSrv = 0, m_segUav = 0;
    uint64_t       m_hdrSrv = 0, m_hdrUav = 0;

    // Per-frame append requests (UPLOAD, SR).
    RHI::GPUBuffer m_requestBuffer;
    void*          m_requestMapped = nullptr;
    uint64_t       m_requestSrv    = 0;

    // Per-frame globals (UPLOAD, CB).
    RHI::GPUBuffer m_systemCB;
    void*          m_systemCBMapped = nullptr;

    // Slot allocator — simple bitset. 64 slots × 1 bit = 8 bytes.
    uint64_t m_slotUsedMask         = 0;
    uint32_t m_maxActiveSlot        = 0;

    // Request bookkeeping
    uint32_t m_requestCountThisFrame = 0;
};
