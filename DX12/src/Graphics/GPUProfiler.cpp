#include "Graphics/GPUProfiler.h"
#include "System/Log.h"
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

void GPUProfiler::Init(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue)
{
    // Query heap
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kMaxQueries;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap))))
    {
        LOG_ERROR("GPUProfiler: CreateQueryHeap failed");
        return;
    }

    // Readback buffer (persistently mapped)
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
    if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_readbackBuffer))))
    {
        LOG_ERROR("GPUProfiler: readback buffer creation failed");
        return;
    }

    // Persistent map
    D3D12_RANGE readRange{ 0, kMaxQueries * sizeof(uint64_t) };
    m_readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedReadback));

    // GPU frequency for tick → ms conversion
    graphicsQueue->GetTimestampFrequency(&m_gpuFrequency);
    if (m_gpuFrequency == 0) m_gpuFrequency = 1;

    LOG_INFO("GPUProfiler: initialised (frequency=%llu, max %u regions)",
             m_gpuFrequency, kMaxRegions);
}

void GPUProfiler::BeginFrame()
{
    // Read previous frame's results from readback buffer.
    hasResults  = false;
    resultCount = 0;
    totalGpuMs  = 0.f;

    if (m_mappedReadback && m_prevRegionCount > 0)
    {
        const double tickToMs = 1000.0 / static_cast<double>(m_gpuFrequency);

        for (uint32_t i = 0; i < m_prevRegionCount; ++i)
        {
            uint64_t begin = m_mappedReadback[i * 2];
            uint64_t end   = m_mappedReadback[i * 2 + 1];
            float ms = (end > begin)
                ? static_cast<float>(static_cast<double>(end - begin) * tickToMs)
                : 0.f;

            results[resultCount].name  = m_regionNames[i]; // still valid (static strings)
            results[resultCount].gpuMs = ms;
            totalGpuMs += ms;
            ++resultCount;
        }
        hasResults = (resultCount > 0);
    }

    // Reset for new frame.
    m_prevRegionCount = m_regionCount.load(std::memory_order_relaxed);
    m_regionCount.store(0, std::memory_order_relaxed);
}

uint32_t GPUProfiler::BeginTimestamp(ID3D12GraphicsCommandList* cmd, const char* name)
{
    uint32_t idx = m_regionCount.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kMaxRegions) return ~0u;

    m_regionNames[idx] = name;
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2);
    return idx;
}

void GPUProfiler::EndTimestamp(ID3D12GraphicsCommandList* cmd, uint32_t regionIndex)
{
    if (regionIndex >= kMaxRegions) return;
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, regionIndex * 2 + 1);
}

void GPUProfiler::ResolveQueries(ID3D12GraphicsCommandList* cmd)
{
    uint32_t count = m_regionCount.load(std::memory_order_relaxed);
    if (count == 0) return;
    if (count > kMaxRegions) count = kMaxRegions;

    cmd->ResolveQueryData(
        m_queryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        0, count * 2,
        m_readbackBuffer.Get(), 0);
}
