#pragma once

// GPUProfiler — DX12 timestamp query profiler.
//
// Records per-pass GPU timestamps via a D3D12 query heap + readback buffer.
// Thread-safe: multiple workers can call BeginTimestamp concurrently (atomic index).
// Results are read back with 1-frame latency (standard for GPU profiling).
//
// Usage:
//   profiler.BeginFrame()                    — read previous results, reset
//   uint32_t r = profiler.BeginTimestamp(cl, "PassName")
//   ... record pass ...
//   profiler.EndTimestamp(cl, r)
//   profiler.ResolveQueries(primaryCL)       — in EndFrame, before Close
//   ... Present + WaitForPreviousFrame ...
//   (next BeginFrame reads the results)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <atomic>

struct GPUProfiler
{
    static constexpr uint32_t kMaxRegions = 32;
    static constexpr uint32_t kMaxQueries = kMaxRegions * 2;

    struct TimingResult
    {
        const char* name = nullptr;
        float       gpuMs = 0.f;
    };

    // ---- Lifecycle ----------------------------------------------------------
    void Init(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue);
    void BeginFrame();                        // read previous results, reset
    void ResolveQueries(ID3D12GraphicsCommandList* cmd); // call in EndFrame

    // ---- Per-pass recording (thread-safe) -----------------------------------
    uint32_t BeginTimestamp(ID3D12GraphicsCommandList* cmd, const char* name);
    void     EndTimestamp(ID3D12GraphicsCommandList* cmd, uint32_t regionIndex);

    // ---- Results (valid after BeginFrame, from previous frame) ---------------
    TimingResult results[kMaxRegions];
    uint32_t     resultCount = 0;
    float        totalGpuMs  = 0.f;
    bool         hasResults  = false;

    bool         enabled     = false;

private:
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_readbackBuffer;
    uint64_t*    m_mappedReadback = nullptr;   // persistently mapped
    uint64_t     m_gpuFrequency  = 1;

    std::atomic<uint32_t> m_regionCount{ 0 };
    const char*           m_regionNames[kMaxRegions]{};
    uint32_t              m_prevRegionCount = 0; // snapshot for readback
};
