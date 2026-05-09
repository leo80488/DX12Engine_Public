#pragma once

// MaterialCBVRing — per-frame upload-heap ring for custom-material CBVs.
//
// Phase E: a custom GBuffer pixel shader can declare its own cbuffer at
// register(b8, space0). The renderer walks ShaderReflect::Reflection each
// frame and packs MaterialComponent::customParams into CPU-mapped upload
// memory; this class owns that memory, rotating through 3 buffers for
// frames-in-flight safety.
//
// Usage:
//     ring.Init(gfx, 256 * 1024);          // once
//     ring.BeginFrame(frameIdx);           // start of each frame
//     auto [cpu, gpu] = ring.Allocate(cbufSize);   // per material
//     memcpy(cpu, packedBytes, cbufSize);
//     // later, per draw:
//     dx12.BindCustomMaterialCBV(gpu, cmdList);
//
// Alignments: D3D12 requires CBV offsets to be 256-byte-aligned. Allocate
// pads the return offset up to the next 256B boundary so downstream binds
// just work.

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

class IGraphicsDevice;

class MaterialCBVRing
{
public:
    // Call once at renderer startup. `bytesPerFrame` is the per-frame capacity;
    // 256KB is enough for ~1000 materials of a ~256-byte CBV each.
    bool Init(IGraphicsDevice& gfx, uint32_t bytesPerFrame = 256 * 1024);
    void Shutdown();

    // Begin a new frame: rotate to the next ring buffer, reset the cursor.
    // frameIdx can be any monotonically-increasing counter — we mod by
    // kFramesInFlight internally.
    void BeginFrame(uint32_t frameIdx);

    // Allocate `sizeBytes` (rounded up to 256B) from the current ring.
    // Returns {nullptr, 0} if the ring is full.
    struct Slice
    {
        void*                       cpu;  // CPU-writable pointer into the upload heap
        D3D12_GPU_VIRTUAL_ADDRESS   gpu;  // GPU virtual address of the same region
    };
    Slice Allocate(uint32_t sizeBytes);

private:
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kCBVAlign       = 256;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffers[kFramesInFlight]{};
    uint8_t*                               m_mapped[kFramesInFlight]{};
    D3D12_GPU_VIRTUAL_ADDRESS              m_baseVA[kFramesInFlight]{};
    uint32_t                               m_bytesPerFrame = 0;
    uint32_t                               m_current       = 0;
    uint32_t                               m_cursor        = 0;
};
