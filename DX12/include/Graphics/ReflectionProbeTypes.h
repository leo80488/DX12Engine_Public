#pragma once

// Reflection-probe shared CPU<->GPU types and renderer-wide constants.
// Mirrors the HLSL `ReflectionProbe` struct in shaders/reflection_probe.hlsli.
//
// The Renderer owns one cubemap-array (Texture2DArray with TEXTURECUBE flag,
// dimensioned 6 * kMaxReflectionProbes slices) and one StructuredBuffer of
// GPUReflectionProbe entries indexed by `cubemapSlice` from the ECS component.

#include <cstdint>
#include <DirectXMath.h>

namespace Reflection
{
    // Cap matches the maximum reasonable probe count for one scene at this
    // tier. Increasing it grows the cubemap-array linearly:
    //   kMaxReflectionProbes * 6 faces * sum(mip texel count) * 8 bytes (FP16x4)
    //   = 64 * 6 * (128*128 + 64*64 + ... + 1*1) * 8 ≈ 67 MiB
    // Bumping to 256 would push it to ~270 MiB — defer until the system is
    // proven and we've added culling.
    inline constexpr uint32_t kMaxReflectionProbes = 64;

    // Per-face cubemap resolution for prefilter mip 0. Matches SkyIBLPass's
    // specular cube so we can reuse its prefilter compute shader byte-for-byte.
    inline constexpr uint32_t kProbeCubemapSize = 128;

    // log2(128) + 1 — same chain depth as SkyIBLPass::kSpecularMips.
    inline constexpr uint32_t kProbeCubemapMips = 7;

    inline constexpr uint32_t kInvalidProbeSlice = ~0u;

    // GPU layout for one probe — 64 bytes (4 × float4 rows). Field order is
    // laid out so HLSL's float3+scalar packing matches without padding
    // inserts. Keep this struct's layout in sync with HLSL.
    //
    // boxMin / boxMax = OUTER falloff box (probe weight reaches 0 at the
    //                    edge). innerExtents = HALF-extents of the full-
    //                    influence inner box (probe weight = 1 inside).
    //                    Both boxes share the probe centre (= position).
    struct alignas(16) GPUReflectionProbe
    {
        DirectX::XMFLOAT3 position;        float influenceRadius; // 16
        DirectX::XMFLOAT3 boxMin;          uint32_t cubemapSlice; // 32
        DirectX::XMFLOAT3 boxMax;          uint32_t flags;        // 48
        // intensity = per-probe brightness multiplier on the sampled radiance.
        // Independent of the sky/atmosphere iblStrength master gate, so a local
        // probe lights surfaces even when sky IBL is fully off. Default 1.0.
        DirectX::XMFLOAT3 innerExtents;    float    intensity;    // 64
    };
    static_assert(sizeof(GPUReflectionProbe) == 64, "GPUReflectionProbe must be 64 bytes");

    // Flag bits stored in GPUReflectionProbe::flags. Mirrors the ECS component.
    enum GPUProbeFlag : uint32_t
    {
        GPU_PROBE_FLAG_BAKED = 1u << 0,
    };
}
