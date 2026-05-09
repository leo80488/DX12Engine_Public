#pragma once

// Per-Stage parameter blocks, decoupled from pass internals. These POD structs
// are the *authoritative* source of user-facing post-process values. Flow:
//
//     EditorLayer UI  ─┐
//     .ippc config    ─┼──►  ParameterStore  ──►  (future: Volume blender) ──►  Adapter Execute  ──►  Pass setters
//     Scripted API    ─┘
//
// Each frame, adapters read their block from the store and push every field
// onto the wrapped pass. Pass internal state thus acts as a write-through
// cache for the most recent store value — which means pass setters still work
// for legacy code paths, but those writes get overwritten on the next frame.

#include "RenderGraph/RenderPass/ColorGradingParams.h"

namespace PostProcess
{

struct CASParams
{
    bool  enabled   = true;
    float sharpness = 0.6f;  // 0..1, AMD CAS convention
};

struct AutoExposureParams
{
    // When false, the pass uploads `manualExposure` to the exposure buffer
    // each frame so downstream Tonemapping still gets a valid value.
    bool  enabled        = true;
    float manualExposure = 1.0f;

    // Tonemapping response time constant (seconds). Adapter derives the
    // per-frame adaptation rate as 1 - exp(-dt / tau). Larger tau = slower.
    float adaptationTau  = 1.5f;

    // Histogram domain (log2-luminance) + percentile clipping window.
    float minLogLuma     = -5.0f;
    float maxLogLuma     =  3.5f;
    float lowPercent     = 0.50f;
    float highPercent    = 0.85f;

    // Final exposure clamp (in linear multiplier space, not EV).
    float minExposure    = 0.10f;
    float maxExposure    = 8.0f;

    // EV compensation applied after the histogram-derived exposure.
    // +1 = 2x brighter, -1 = 2x darker. Useful when a bright IBL sky crushes
    // subject exposure.
    float evBias         = 0.0f;

    // Middle-grey target. Default 0.18 matches photographic convention.
    float keyValue       = 0.18f;
};

// Bloom has no user-facing parameters yet — the pass is entirely driven by
// scene HDR input + intensity applied downstream in Tonemapping. Reserved
// for future knobs (threshold, radius, per-mip weights) once we expose them.
struct BloomParams
{
};

struct TonemappingParams
{
    // Post-tonemap bloom composite weight (linear). 0 = no bloom, 1 = full.
    float              bloomStrength       = 0.04f;
    bool               colorGradingEnabled = true;
    // Full lift/gamma/gain + temperature/tint/saturation/etc. struct.
    ColorGradingParams grading             = {};
};

} // namespace PostProcess
