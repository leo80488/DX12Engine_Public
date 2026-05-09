#pragma once

// ColorGradingParams — CPU-side parameters for procedural 3D LUT generation.
// Must match the HLSL cbuffer layout in GenerateLUT.cs.hlsl (16-byte aligned).

#include <DirectXMath.h>
#include <cstring>

struct alignas(16) ColorGradingParams
{
    // --- Exposure & Contrast ---
    float exposure   = 0.0f;   // EV offset
    float contrast   = 1.0f;   // 1 = neutral, >1 = more contrast
    float brightness = 0.0f;   // additive offset
    float _pad0      = 0.0f;

    // --- Color Balance (Lift / Gamma / Gain) ---
    DirectX::XMFLOAT3 lift  = { 0.f, 0.f, 0.f };   // shadows offset
    float _pad1 = 0.f;
    DirectX::XMFLOAT3 gamma = { 1.f, 1.f, 1.f };   // midtones power
    float _pad2 = 0.f;
    DirectX::XMFLOAT3 gain  = { 1.f, 1.f, 1.f };   // highlights multiply
    float _pad3 = 0.f;

    // --- HSL ---
    float hueShift   = 0.0f;   // degrees, -180..180
    float saturation = 1.0f;   // 1 = neutral
    float vibrance   = 0.0f;   // only boosts low-sat regions
    float _pad4      = 0.0f;

    // --- White Balance ---
    float temperature = 0.0f;  // -1..+1 (cool..warm)
    float tint        = 0.0f;  // -1..+1 (green..magenta)
    float _pad5[2]    = {};

    // --- Vignette & Grain ---
    float vignetteStrength = 0.0f;
    float filmGrain        = 0.0f;
    float _pad6[2]         = {};

    bool operator==(const ColorGradingParams& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    bool operator!=(const ColorGradingParams& o) const { return !(*this == o); }
};

static_assert(sizeof(ColorGradingParams) % 16 == 0, "ColorGradingParams must be 16-byte aligned");
