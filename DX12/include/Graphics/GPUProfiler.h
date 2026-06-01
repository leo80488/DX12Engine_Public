#pragma once

// GPUProfiler — DX12 timestamp query profiler.
//
// Records per-pass GPU timestamps via a D3D12 query heap + readback buffer.
// Thread-safe: multiple workers can call BeginTimestamp concurrently (atomic index).
// Results are read back with kFrameCount-1 frames latency (matches the engine's
// triple-buffer pipelining — see GraphicsDX12::BeginFrame).
//
// Each region is tagged with the queue type that recorded it. Per-queue
// totals are summed and the "effective frame" time = max(graphics, compute)
// (the critical path when graphics and compute queues overlap; their work
// times can't simply be added because they execute in parallel).
//
// Per-slot ring: readback buffer + names + queues are kept per-frame-slot
// so pipelined CPU/GPU overlap doesn't race with the readback memcpy. Slot
// indices match GraphicsDX12::m_frameIndex (the swap-chain backbuffer index).
//
// Usage:
//   profiler.BeginFrame(slot)            — read slot's results (resolved
//                                          kFrameCount-1 frames ago), reset
//                                          the live region counter for the
//                                          new frame
//   uint32_t r = profiler.BeginTimestamp(cl, "PassName", queueType)
//   ... record pass ...
//   profiler.EndTimestamp(cl, r)
//   profiler.ResolveQueries(primaryCL, slot)  — call in EndFrame, writes
//                                                slot's readback buffer
//   ... Present + signal fence ...
//   (kFrameCount-1 frames later, BeginFrame(slot) reads the results)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <atomic>

struct GPUProfiler
{
    static constexpr uint32_t kMaxRegions = 128;
    static constexpr uint32_t kMaxQueries = kMaxRegions * 2;
    // Per-frame ring depth — MUST match GraphicsDX12::FrameCount. With CPU
    // running kFrameCount-1 frames ahead of GPU, a single shared readback
    // buffer would race (BeginFrame reads while GPU is still writing the
    // mid-flight resolve). Per-slot rings eliminate that hazard.
    static constexpr uint32_t kFrameCount = 3;

    // Mirror of RHI::QUEUE_TYPE without dragging in GraphicsStruct.h.
    // GRAPHICS=0, COMPUTE=1, COPY=2 — must stay in sync.
    static constexpr uint8_t kQueueGraphics = 0;
    static constexpr uint8_t kQueueCompute  = 1;
    static constexpr uint8_t kQueueCopy     = 2;
    static constexpr uint8_t kQueueCount    = 3;

    struct TimingResult
    {
        const char* name      = nullptr;
        float       gpuMs     = 0.f;
        uint8_t     queueType = kQueueGraphics;
    };

    // ---- Lifecycle ----------------------------------------------------------
    // All three queues are needed so we can read each one's timestamp
    // frequency (graphics + compute queue frequencies may differ on some
    // hardware; compute or copy queue may be null on some adapters).
    void Init(ID3D12Device* device,
              ID3D12CommandQueue* graphicsQueue,
              ID3D12CommandQueue* computeQueue,
              ID3D12CommandQueue* copyQueue);

    // Read previous results from slot's readback buffer, reset live counter.
    // Caller MUST guarantee the slot's resolve has completed on the GPU (by
    // waiting on the slot's fence value before invoking).
    void BeginFrame(uint32_t slot);

    // Resolve live queries into slot's readback buffer at end of frame.
    void ResolveQueries(ID3D12GraphicsCommandList* cmd, uint32_t slot);

    // ---- Per-pass recording (thread-safe) -----------------------------------
    // queueType is the RHI::QUEUE_TYPE of the CL recording the timestamp.
    // Used so per-queue totals + the critical-path effective frame time can
    // be computed without parsing CL state at readback.
    uint32_t BeginTimestamp(ID3D12GraphicsCommandList* cmd,
                            const char* name,
                            uint8_t queueType);
    void     EndTimestamp(ID3D12GraphicsCommandList* cmd, uint32_t regionIndex);

    // ---- Results (valid after BeginFrame, from previous frame) ---------------
    TimingResult results[kMaxRegions];
    uint32_t     resultCount = 0;

    // totalGpuMs is the SUM of every region (legacy callers still read it).
    // perQueueTotalMs[q] is the sum of regions on queue q only.
    // effectiveFrameMs is the critical-path estimate = max over queues —
    // i.e. how long the GPU is actually busy in the steady state when
    // graphics + compute queues overlap.
    float        totalGpuMs              = 0.f;
    float        perQueueTotalMs[kQueueCount] {};
    float        effectiveFrameMs        = 0.f;
    bool         hasResults              = false;

    bool         enabled                 = false;

private:
    // One readback + name/queue snapshot per in-flight frame. The live
    // counter (m_regionCount) is shared — atomic across worker threads — and
    // snapshotted into the slot at ResolveQueries time.
    struct FrameSlot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource>  readbackBuffer;
        uint64_t*    mappedReadback   = nullptr;
        const char*  regionNames [kMaxRegions] {};
        uint8_t      regionQueues[kMaxRegions] {};
        uint32_t     regionCount      = 0; // snapshot taken at ResolveQueries
    };
    FrameSlot              m_slots[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    uint64_t               m_queueFrequency[kQueueCount]{ 1, 1, 1 };

    // Live counter — written by BeginTimestamp on any worker thread, read +
    // snapshotted into the current slot at ResolveQueries.
    std::atomic<uint32_t>  m_regionCount{ 0 };

    // Slot currently being recorded. Set at BeginFrame, used by
    // BeginTimestamp (writes to slot's name/queue arrays) and ResolveQueries
    // (snapshots regionCount into slot).
    uint32_t               m_recordSlot   = 0;
};
