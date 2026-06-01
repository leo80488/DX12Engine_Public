#pragma once

// GPUInstanceData — per-instance data uploaded to GPU StructuredBuffer.
// Must be byte-for-byte identical to shaders/gpu_instance.hlsli GPUInstanceData.

#include <DirectXMath.h>
#include <cstdint>

struct GPUInstanceData
{
    DirectX::XMFLOAT4X4 world;          // 64 bytes — transposed world matrix
    uint32_t            meshDescIdx;    //  4 bytes
    uint32_t            materialIdx;    //  4 bytes
    uint32_t            lodLevel;       //  4 bytes
    uint32_t            pad;            //  4 bytes
    DirectX::XMFLOAT4X4 prevWorld;      // 64 bytes — prev-frame world (for TAA velocity)
};                                       // total: 144 bytes

static_assert(sizeof(GPUInstanceData) == 144, "GPUInstanceData must be 144 bytes");
