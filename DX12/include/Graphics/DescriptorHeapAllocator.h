#pragma once

// DescriptorHeapAllocator — two-tier CPU/GPU descriptor heap manager.
//
// For shader-visible types (CBV_SRV_UAV, SAMPLER):
//   CPU staging heap  — small, non-shader-visible.  Used to build descriptors
//                       with CreateSRV / CreateCBV etc.
//   GPU heap          — large, shader-visible.  Divided into two regions:
//     [0 .. staticCap-1]          Static region  — persistent allocations.
//     [staticCap .. total-1]      Dynamic region — per-frame ring buffer.
//   After writing to the CPU staging handle, call allocation.CopyToGpu() to
//   CopyDescriptorsSimple() the descriptor into the GPU static region.
//   AllocateDynamic() copies CPU descriptors directly into the ring buffer.
//
// For CPU-only types (RTV, DSV):
//   Only the CPU heap is created.  No GPU heap or dynamic ring buffer.
//   AllocateDynamic() is invalid for these types.
//
// Layout:
//   Init(device, type, cpuStagingCount, gpuStaticCount, gpuDynamicCount)
//   gpuStaticCount = gpuDynamicCount = 0  →  CPU-only allocator (RTV / DSV).
//
// Thread safety: none.  Callers must serialize access.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <deque>
#include <cstdint>

class DescriptorHeapAllocator;

// ---------------------------------------------------------------------------
// DescriptorAllocation
// Returned by AllocateStatic / Allocate.  Holds both CPU staging and GPU
// static handles for the same logical descriptor slot(s).
// ---------------------------------------------------------------------------
struct DescriptorAllocation
{
    DescriptorAllocation() = default;

    // CPU staging heap CPU handle — pass to CreateSRV / CreateCBV / etc.
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle() const;

    // GPU heap CPU handle — used for direct GPU-heap writes (e.g. ImGui callbacks)
    // or as the CopyDescriptorsSimple destination.
    D3D12_CPU_DESCRIPTOR_HANDLE GetGpuCpuHandle() const;

    // GPU heap GPU handle — pass to SetGraphicsRootDescriptorTable.
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() const;

    UINT GetCount()          const;
    bool IsShaderVisible()   const;
    bool IsValid()           const;

    // After writing descriptors via GetCpuHandle(), call this to
    // CopyDescriptorsSimple from CPU staging → GPU static region.
    // No-op for CPU-only allocators (RTV / DSV).
    void CopyToGpu();

    // Return all slots to the allocator's free lists.
    void Free();

private:
    friend class DescriptorHeapAllocator;

    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuHandle    {};   // CPU staging
    D3D12_CPU_DESCRIPTOR_HANDLE m_gpuCpuHandle {};   // GPU heap CPU  (shader-visible types only)
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuHandle    {};   // GPU heap GPU  (shader-visible types only)

    UINT  m_cpuIndex  { 0 };
    UINT  m_gpuIndex  { 0 };
    UINT  m_count     { 0 };
    bool  m_shaderVisible { false };

    DescriptorHeapAllocator* m_allocator { nullptr };
};

// ---------------------------------------------------------------------------
// DescriptorHeapAllocator
// ---------------------------------------------------------------------------
class DescriptorHeapAllocator
{
public:
    DescriptorHeapAllocator() = default;

    // cpuStagingCount  — number of non-shader-visible CPU-side staging slots.
    // gpuStaticCount   — shader-visible GPU static region size.
    // gpuDynamicCount  — shader-visible GPU ring-buffer region size.
    // For RTV / DSV pass gpuStaticCount = gpuDynamicCount = 0.
    void Init(
        ID3D12Device*              device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT                       cpuStagingCount,
        UINT                       gpuStaticCount,
        UINT                       gpuDynamicCount);

    // ---- Static allocation -------------------------------------------------

    // Allocate 'count' contiguous slots in the static region.
    // Returns a DescriptorAllocation with both CPU staging and GPU handles.
    // Call allocation.CopyToGpu() after writing descriptors to the CPU handle.
    DescriptorAllocation Allocate(UINT count);          // alias for AllocateStatic
    DescriptorAllocation AllocateStatic(UINT count);

    // ---- Dynamic ring-buffer allocation ------------------------------------

    // Copy 'count' descriptors from 'srcCpuHandle' into the GPU dynamic
    // ring-buffer region and return the GPU handle for this frame's draw.
    // fenceValue — the fence value that will be signaled after the GPU work
    //              that uses this allocation is submitted.
    // Returns an invalid handle ({0}) if the ring buffer is full.
    D3D12_GPU_DESCRIPTOR_HANDLE AllocateDynamic(
        UINT                        count,
        D3D12_CPU_DESCRIPTOR_HANDLE srcCpuHandle,
        UINT64                      fenceValue);

    // Reclaim dynamic slots whose fenceValue <= completedFence.
    void ReclaimDynamic(UINT64 completedFence);

    // ---- Heap accessors ----------------------------------------------------

    // Primary heap:  GPU heap for shader-visible types, CPU heap otherwise.
    // Suitable for ID3D12GraphicsCommandList::SetDescriptorHeaps.
    ID3D12DescriptorHeap* GetHeap()    const;

    // GPU-visible heap (nullptr for CPU-only types).
    ID3D12DescriptorHeap* GetGpuHeap() const;

    // CPU staging heap.
    ID3D12DescriptorHeap* GetCpuHeap() const;

    UINT GetDescriptorSize() const;
    bool IsShaderVisible()   const;
    UINT GetCapacity()       const;   // GPU total (or CPU total for CPU-only)

    // ---- Internal (called by DescriptorAllocation) -------------------------
    void FreeStatic(UINT cpuIndex, UINT gpuIndex, UINT count);
    void CopyToGpu (UINT cpuIndex, UINT gpuIndex, UINT count);

private:
    struct FreeBlock   { UINT startIndex; UINT count; };
    struct DynamicEntry{ UINT64 fenceValue; UINT physOffset; UINT count; };

    // ---- CPU staging heap (always present) -
    ID3D12Device*                               m_device         { nullptr };
    D3D12_DESCRIPTOR_HEAP_TYPE                  m_type           {};
    UINT                                        m_descriptorSize { 0 };
    bool                                        m_shaderVisible  { false };

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cpuHeap;
    UINT m_cpuCapacity  { 0 };
    UINT m_cpuNextFree  { 0 };
    std::vector<FreeBlock> m_cpuFreeList;

    // ---- GPU shader-visible heap (shader-visible types only) ---------------
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gpuHeap;
    UINT m_gpuStaticCapacity  { 0 };
    UINT m_gpuDynamicCapacity { 0 };

    // GPU static: linear + free-list
    UINT m_gpuStaticNextFree { 0 };
    std::vector<FreeBlock> m_gpuStaticFreeList;

    // GPU dynamic: ring buffer
    // Physical indices in GPU heap: [m_gpuStaticCapacity .. gpuTotal-1]
    UINT m_dynamicHead { 0 };   // next write offset (0-based within dynamic region)
    UINT m_dynamicUsed { 0 };   // total slots currently in-flight
    std::deque<DynamicEntry> m_dynamicQueue;
};
