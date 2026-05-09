#include "Graphics/DescriptorHeapAllocator.h"
#include <cassert>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// DescriptorAllocation
// ---------------------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocation::GetCpuHandle() const
{
    return m_cpuHandle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocation::GetGpuCpuHandle() const
{
    return m_gpuCpuHandle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorAllocation::GetGpuHandle() const
{
    return m_gpuHandle;
}

UINT DescriptorAllocation::GetCount() const
{
    return m_count;
}

bool DescriptorAllocation::IsShaderVisible() const
{
    return m_shaderVisible;
}

bool DescriptorAllocation::IsValid() const
{
    return m_allocator != nullptr && m_count > 0;
}

void DescriptorAllocation::CopyToGpu()
{
    if (!m_allocator || !m_shaderVisible || m_count == 0)
        return;
    m_allocator->CopyToGpu(m_cpuIndex, m_gpuIndex, m_count);
}

void DescriptorAllocation::Free()
{
    if (m_allocator && m_count > 0)
    {
        m_allocator->FreeStatic(m_cpuIndex, m_gpuIndex, m_count);
        m_allocator = nullptr;
        m_count     = 0;
    }
}

// ---------------------------------------------------------------------------
// DescriptorHeapAllocator
// ---------------------------------------------------------------------------

void DescriptorHeapAllocator::Init(
    ID3D12Device*              device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT                       cpuStagingCount,
    UINT                       gpuStaticCount,
    UINT                       gpuDynamicCount)
{
    m_device         = device;
    m_type           = type;
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
    m_shaderVisible  = (gpuStaticCount + gpuDynamicCount) > 0;

    // CPU staging heap (always created)
    m_cpuCapacity = cpuStagingCount;
    m_cpuNextFree = 0;
    m_cpuFreeList.clear();
    m_cpuHeap.Reset();

    if (cpuStagingCount > 0)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = type;
        desc.NumDescriptors = cpuStagingCount;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_cpuHeap));
    }

    // GPU shader-visible heap
    m_gpuStaticCapacity  = gpuStaticCount;
    m_gpuDynamicCapacity = gpuDynamicCount;
    m_gpuStaticNextFree  = 0;
    m_gpuStaticFreeList.clear();
    m_dynamicHead  = 0;
    m_dynamicUsed  = 0;
    m_dynamicQueue.clear();
    m_gpuHeap.Reset();

    if (m_shaderVisible)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = type;
        desc.NumDescriptors = gpuStaticCount + gpuDynamicCount;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_gpuHeap));
    }
}

// ---- Static allocation ---------------------------------------------------

DescriptorAllocation DescriptorHeapAllocator::AllocateStatic(UINT count)
{
    DescriptorAllocation result;
    if (!m_cpuHeap || count == 0)
        return result;

    // --- CPU staging slot ---
    UINT cpuIndex = ~0u;
    for (size_t i = 0; i < m_cpuFreeList.size(); ++i)
    {
        auto& blk = m_cpuFreeList[i];
        if (blk.count >= count)
        {
            cpuIndex = blk.startIndex;
            if (blk.count == count) { m_cpuFreeList[i] = m_cpuFreeList.back(); m_cpuFreeList.pop_back(); }
            else                    { blk.startIndex += count; blk.count -= count; }
            break;
        }
    }
    if (cpuIndex == ~0u)
    {
        if (m_cpuNextFree + count > m_cpuCapacity)
            return result;
        cpuIndex = m_cpuNextFree;
        m_cpuNextFree += count;
    }

    // --- GPU static slot (shader-visible types only) ---
    UINT gpuIndex = 0;
    if (m_shaderVisible)
    {
        gpuIndex = ~0u;
        for (size_t i = 0; i < m_gpuStaticFreeList.size(); ++i)
        {
            auto& blk = m_gpuStaticFreeList[i];
            if (blk.count >= count)
            {
                gpuIndex = blk.startIndex;
                if (blk.count == count) { m_gpuStaticFreeList[i] = m_gpuStaticFreeList.back(); m_gpuStaticFreeList.pop_back(); }
                else                    { blk.startIndex += count; blk.count -= count; }
                break;
            }
        }
        if (gpuIndex == ~0u)
        {
            if (m_gpuStaticNextFree + count > m_gpuStaticCapacity)
                return result;
            gpuIndex = m_gpuStaticNextFree;
            m_gpuStaticNextFree += count;
        }
    }

    // --- Build handles ---
    result.m_cpuHandle = m_cpuHeap->GetCPUDescriptorHandleForHeapStart();
    result.m_cpuHandle.ptr += static_cast<SIZE_T>(cpuIndex) * m_descriptorSize;

    if (m_shaderVisible && m_gpuHeap)
    {
        result.m_gpuCpuHandle = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
        result.m_gpuCpuHandle.ptr += static_cast<SIZE_T>(gpuIndex) * m_descriptorSize;
        result.m_gpuHandle = m_gpuHeap->GetGPUDescriptorHandleForHeapStart();
        result.m_gpuHandle.ptr += static_cast<SIZE_T>(gpuIndex) * m_descriptorSize;
    }

    result.m_cpuIndex      = cpuIndex;
    result.m_gpuIndex      = gpuIndex;
    result.m_count         = count;
    result.m_shaderVisible = m_shaderVisible;
    result.m_allocator     = this;
    return result;
}

DescriptorAllocation DescriptorHeapAllocator::Allocate(UINT count)
{
    return AllocateStatic(count);
}

// ---- Dynamic ring-buffer allocation --------------------------------------

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapAllocator::AllocateDynamic(
    UINT                        count,
    D3D12_CPU_DESCRIPTOR_HANDLE srcCpuHandle,
    UINT64                      fenceValue)
{
    D3D12_GPU_DESCRIPTOR_HANDLE invalid{};
    if (!m_shaderVisible || !m_gpuHeap || count == 0 || count > m_gpuDynamicCapacity)
        return invalid;

    const UINT capacity = m_gpuDynamicCapacity;
    const UINT base     = m_gpuStaticCapacity;   // dynamic region starts here in GPU heap

    UINT physOffset = m_dynamicHead;

    if (physOffset + count > capacity)
    {
        // Not enough contiguous space at end — waste tail slots and wrap to 0.
        const UINT waste = capacity - physOffset;
        if (m_dynamicUsed + waste + count > capacity)
            return invalid;   // can't fit even after wrapping

        if (waste > 0)
            m_dynamicQueue.push_back({ fenceValue, physOffset, waste });
        m_dynamicUsed += waste;
        m_dynamicHead  = 0;
        physOffset     = 0;
    }
    else
    {
        if (m_dynamicUsed + count > capacity)
            return invalid;
    }

    // Copy srcCpuHandle → GPU dynamic region
    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += static_cast<SIZE_T>(base + physOffset) * m_descriptorSize;
    m_device->CopyDescriptorsSimple(count, dst, srcCpuHandle, m_type);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_gpuHeap->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += static_cast<SIZE_T>(base + physOffset) * m_descriptorSize;

    m_dynamicQueue.push_back({ fenceValue, physOffset, count });
    m_dynamicUsed += count;
    m_dynamicHead  = physOffset + count;
    if (m_dynamicHead >= capacity) m_dynamicHead = 0;

    return gpuHandle;
}

void DescriptorHeapAllocator::ReclaimDynamic(UINT64 completedFence)
{
    while (!m_dynamicQueue.empty())
    {
        const auto& front = m_dynamicQueue.front();
        if (front.fenceValue > completedFence)
            break;
        m_dynamicUsed -= front.count;
        m_dynamicQueue.pop_front();
    }
}

// ---- Internal (called by DescriptorAllocation) ---------------------------

void DescriptorHeapAllocator::FreeStatic(UINT cpuIndex, UINT gpuIndex, UINT count)
{
    if (count == 0) return;
    m_cpuFreeList.push_back({ cpuIndex, count });
    if (m_shaderVisible)
        m_gpuStaticFreeList.push_back({ gpuIndex, count });
}

void DescriptorHeapAllocator::CopyToGpu(UINT cpuIndex, UINT gpuIndex, UINT count)
{
    if (!m_shaderVisible || !m_cpuHeap || !m_gpuHeap || count == 0)
        return;

    D3D12_CPU_DESCRIPTOR_HANDLE src = m_cpuHeap->GetCPUDescriptorHandleForHeapStart();
    src.ptr += static_cast<SIZE_T>(cpuIndex) * m_descriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += static_cast<SIZE_T>(gpuIndex) * m_descriptorSize;

    m_device->CopyDescriptorsSimple(count, dst, src, m_type);
}

// ---- Heap accessors ------------------------------------------------------

ID3D12DescriptorHeap* DescriptorHeapAllocator::GetHeap() const
{
    return m_shaderVisible ? m_gpuHeap.Get() : m_cpuHeap.Get();
}

ID3D12DescriptorHeap* DescriptorHeapAllocator::GetGpuHeap() const
{
    return m_gpuHeap.Get();
}

ID3D12DescriptorHeap* DescriptorHeapAllocator::GetCpuHeap() const
{
    return m_cpuHeap.Get();
}

UINT DescriptorHeapAllocator::GetDescriptorSize() const
{
    return m_descriptorSize;
}

bool DescriptorHeapAllocator::IsShaderVisible() const
{
    return m_shaderVisible;
}

UINT DescriptorHeapAllocator::GetCapacity() const
{
    return m_shaderVisible ? (m_gpuStaticCapacity + m_gpuDynamicCapacity) : m_cpuCapacity;
}
