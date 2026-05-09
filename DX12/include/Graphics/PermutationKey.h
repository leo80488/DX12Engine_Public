#pragma once
#include <cstdint>

// PermutationKey — bitmask encoding active #define features for a shader variant.
// Each bit maps to one compile-time define passed to D3DCompile / DXC.
// Derive from MaterialData in the Renderer layer; never expose to ECS.
struct PermutationKey
{
    uint32_t bits = 0;

    // Feature flags (add per-shader-family constants below).
    static constexpr uint32_t HAS_NORMALMAP      = 1u << 0;
    static constexpr uint32_t HAS_EMISSIVE       = 1u << 1;
    static constexpr uint32_t ALPHA_TEST         = 1u << 2;  // clip(alpha - alphaRef) in deferred GBuffer
    static constexpr uint32_t DOUBLE_SIDED       = 1u << 3;
    static constexpr uint32_t SKINNED            = 1u << 4;
    static constexpr uint32_t ALPHA_BLEND        = 1u << 5;  // forward transparent (SRC_ALPHA/INV_SRC_ALPHA)
    static constexpr uint32_t ADDITIVE_BLEND     = 1u << 6;  // forward transparent additive (ONE/ONE)
    static constexpr uint32_t PREMULTIPLIED_BLEND= 1u << 7;  // forward transparent premultiplied (ONE/INV_SRC_ALPHA)
    static constexpr uint32_t MULTIPLY_BLEND     = 1u << 8;  // forward transparent multiply (DST_COLOR/INV_SRC_ALPHA)
    static constexpr uint32_t BILLBOARD          = 1u << 9;  // camera-facing quad (VS ignores world rotation)
    static constexpr uint32_t UNLIT              = 1u << 10; // skip lighting, output albedo directly
    static constexpr uint32_t NPR_STENCIL       = 1u << 11; // lighting pass: stencil-gated NPR variant
    static constexpr uint32_t OCCLUSION_CULL   = 1u << 12; // culling pass: enable Hi-Z occlusion test
    void Set(uint32_t feature, bool val) noexcept
    { bits = val ? (bits | feature) : (bits & ~feature); }
    bool Has(uint32_t feature) const noexcept { return (bits & feature) != 0; }

    bool operator==(const PermutationKey& o) const noexcept { return bits == o.bits; }
    bool operator!=(const PermutationKey& o) const noexcept { return bits != o.bits; }
};
