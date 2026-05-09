#pragma once

// MaterialSRVRing — per-frame descriptor ring for custom-material texture
// tables (Phase F). Mirrors MaterialCBVRing but for CBV_SRV_UAV descriptors:
// at `BeginFrame` the cursor resets into the current frame's slice of a
// static pre-allocation, and `Allocate(count)` hands out a contiguous run
// of shader-visible descriptors.
//
// Usage:
//     ring.Init(gfx, 128 /* max materials */);
//     ring.BeginFrame(frameIdx);
//     auto slice = ring.Allocate(4);
//     // Copy each texture's SRV CPU handle into slice.cpu + i * descInc;
//     dx12.BindCustomMaterialTextureTable(slice.gpu, cmdList);

#include <cstdint>
#include <d3d12.h>

#include "Graphics/DescriptorHeapAllocator.h"

class IGraphicsDevice;

class MaterialSRVRing
{
public:
    // Reserves enough descriptors for `maxMatsPerFrame` materials, each with
    // GraphicsDX12::kCustomMatTextureSlots descriptors, across 3 frames.
    bool Init(IGraphicsDevice& gfx, uint32_t maxMatsPerFrame = 128);
    void Shutdown();

    void BeginFrame(uint32_t frameIdx);

    struct Slice
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;  // starting CPU handle (for CopyDescriptors)
        uint64_t                    gpu;  // starting GPU handle (bind target)
        uint32_t                    descIncBytes;  // advance between slots
    };

    // Allocate `count` contiguous descriptors from the current frame's ring
    // slice. Returns {0,0,0} if full.
    Slice Allocate(uint32_t count);

    // Populates a slot with the default white-texture descriptor so unused
    // custom-texture slots don't sample garbage. Caller does this on init
    // for the "fallback descriptor" that Allocate uses internally.
    //
    // Not used internally yet — exposed for the Renderer to set up at startup.
    D3D12_CPU_DESCRIPTOR_HANDLE GetFallbackCpuHandle() const { return m_fallbackCpu; }
    void SetFallbackCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE h) { m_fallbackCpu = h; }

private:
    static constexpr uint32_t kFramesInFlight = 3;

    DescriptorAllocation      m_frames[kFramesInFlight]{};
    uint32_t                  m_descPerFrame   = 0;
    uint32_t                  m_descIncBytes   = 0;
    uint32_t                  m_cursor         = 0;
    uint32_t                  m_current        = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE m_fallbackCpu  = {};
};
