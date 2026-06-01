#include "Graphics/GPUProfiler.h"
#include "System/Log.h"
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

void GPUProfiler::Init(ID3D12Device* device,
                       ID3D12CommandQueue* graphicsQueue,
                       ID3D12CommandQueue* computeQueue,
                       ID3D12CommandQueue* copyQueue)
{
    // Single shared query heap — slot-aware reads are done via per-slot
    // readback buffers (ResolveQueryData copies the live query slot into
    // the active slot's buffer, so a single heap is sufficient as long as
    // the regionCount snapshot per slot tells us how much to read).
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kMaxQueries;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap))))
    {
        LOG_ERROR("GPUProfiler: CreateQueryHeap failed");
        return;
    }

    // Per-slot readback buffers (persistently mapped). FrameCount of them so
    // BeginFrame N can safely read slot[N % FrameCount] (whose write completed
    // kFrameCount-1 frames ago) while frames N-1 / N-2's resolve copies are
    // still in flight against their own slots.
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width              = kMaxQueries * sizeof(uint64_t);
    bufDesc.Height             = 1;
    bufDesc.DepthOrArraySize   = 1;
    bufDesc.MipLevels          = 1;
    bufDesc.SampleDesc.Count   = 1;
    bufDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (uint32_t s = 0; s < kFrameCount; ++s)
    {
        FrameSlot& slot = m_slots[s];
        if (FAILED(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&slot.readbackBuffer))))
        {
            LOG_ERROR("GPUProfiler: readback buffer[%u] creation failed", s);
            return;
        }
        D3D12_RANGE readRange{ 0, kMaxQueries * sizeof(uint64_t) };
        slot.readbackBuffer->Map(0, &readRange,
            reinterpret_cast<void**>(&slot.mappedReadback));
    }

    // Per-queue timestamp frequency. Graphics + compute may differ on some
    // hardware. Copy queue often has its own (lower) frequency, but we don't
    // currently profile copy regions; we cache it for completeness.
    ID3D12CommandQueue* queues[kQueueCount] = { graphicsQueue, computeQueue, copyQueue };
    for (uint8_t q = 0; q < kQueueCount; ++q)
    {
        m_queueFrequency[q] = 1;
        if (queues[q])
        {
            uint64_t f = 0;
            if (SUCCEEDED(queues[q]->GetTimestampFrequency(&f)) && f != 0)
                m_queueFrequency[q] = f;
        }
    }

    LOG_INFO("GPUProfiler: initialised (gfx=%llu compute=%llu copy=%llu, max %u regions, %u slots)",
             m_queueFrequency[kQueueGraphics],
             m_queueFrequency[kQueueCompute],
             m_queueFrequency[kQueueCopy],
             kMaxRegions, kFrameCount);
}

void GPUProfiler::BeginFrame(uint32_t slot)
{
    // Read previous results from slot's readback buffer. Caller has waited
    // on the slot's fence value before invoking, so the resolve from
    // kFrameCount-1 frames ago is guaranteed complete.
    hasResults  = false;
    resultCount = 0;
    totalGpuMs  = 0.f;
    for (uint8_t q = 0; q < kQueueCount; ++q) perQueueTotalMs[q] = 0.f;

    if (slot >= kFrameCount) slot = 0;
    FrameSlot& s = m_slots[slot];

    if (s.mappedReadback && s.regionCount > 0)
    {
        for (uint32_t i = 0; i < s.regionCount; ++i)
        {
            const uint8_t qt = s.regionQueues[i];
            const uint64_t freq = (qt < kQueueCount)
                ? m_queueFrequency[qt]
                : m_queueFrequency[kQueueGraphics];
            const double tickToMs = 1000.0 / static_cast<double>(freq);

            uint64_t begin = s.mappedReadback[i * 2];
            uint64_t end   = s.mappedReadback[i * 2 + 1];
            float ms = (end > begin)
                ? static_cast<float>(static_cast<double>(end - begin) * tickToMs)
                : 0.f;

            results[resultCount].name      = s.regionNames[i]; // still valid (static strings)
            results[resultCount].gpuMs     = ms;
            results[resultCount].queueType = qt;
            totalGpuMs += ms;
            if (qt < kQueueCount) perQueueTotalMs[qt] += ms;
            ++resultCount;
        }
        hasResults = (resultCount > 0);
    }

    // Effective frame = critical path under the assumption that graphics +
    // compute queues run in parallel between cross-queue fences. Copy queue
    // is included for completeness, though we don't currently profile it.
    effectiveFrameMs = perQueueTotalMs[kQueueGraphics];
    if (perQueueTotalMs[kQueueCompute] > effectiveFrameMs)
        effectiveFrameMs = perQueueTotalMs[kQueueCompute];
    if (perQueueTotalMs[kQueueCopy] > effectiveFrameMs)
        effectiveFrameMs = perQueueTotalMs[kQueueCopy];

    // Start a new recording window on this slot.
    m_recordSlot = slot;
    m_regionCount.store(0, std::memory_order_relaxed);
}

uint32_t GPUProfiler::BeginTimestamp(ID3D12GraphicsCommandList* cmd,
                                     const char* name,
                                     uint8_t queueType)
{
    uint32_t idx = m_regionCount.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kMaxRegions) return ~0u;

    FrameSlot& s = m_slots[m_recordSlot];
    s.regionNames[idx]  = name;
    s.regionQueues[idx] = (queueType < kQueueCount) ? queueType : kQueueGraphics;
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2);
    return idx;
}

void GPUProfiler::EndTimestamp(ID3D12GraphicsCommandList* cmd, uint32_t regionIndex)
{
    if (regionIndex >= kMaxRegions) return;
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, regionIndex * 2 + 1);
}

void GPUProfiler::ResolveQueries(ID3D12GraphicsCommandList* cmd, uint32_t slot)
{
    uint32_t count = m_regionCount.load(std::memory_order_relaxed);
    if (count == 0) return;
    if (count > kMaxRegions) count = kMaxRegions;
    if (slot >= kFrameCount) slot = 0;

    FrameSlot& s = m_slots[slot];
    if (!s.readbackBuffer) return;

    cmd->ResolveQueryData(
        m_queryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        0, count * 2,
        s.readbackBuffer.Get(), 0);

    // Snapshot the live count + names+queues are already written in the
    // slot's arrays during BeginTimestamp (m_recordSlot was set to this
    // slot in BeginFrame). The next BeginFrame on this slot — kFrameCount-1
    // frames later — uses regionCount to know how much of the buffer is
    // populated.
    s.regionCount = count;
}
