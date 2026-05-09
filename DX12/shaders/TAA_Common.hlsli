#ifndef TAA_COMMON_HLSLI
#define TAA_COMMON_HLSLI

// -----------------------------------------------------------------------------
// TAA_Common.hlsli
//
// Shared TAACB and pure helper functions for the TAA resolve pipeline.
// Helpers here have NO texture/sampler access — they take values, not handles —
// so this header can be included anywhere without claiming register slots.
//
// CB layout MUST match TAAPass::TAACB on the C++ side, and the field order is
// the contract. If you reorder fields, update TAAPass.h in lockstep.
// -----------------------------------------------------------------------------

// ---- Build-time switch ------------------------------------------------------
// 1 (default) — Karis tonemap-blend pipeline:
//                 currT = ToneMapLuma(curr); hist = ToneMapLuma(hist);
//                 blend in tonemapped space; output = InvToneMapLuma(resolved).
//                 Robust against HDR fireflies; matches the Fix I/J thresholds.
// 0           — Falcor-style linear blend (no tonemap around the blend).
//                 Lighter, but firefly suppression weakens — Fix I/J HDR ratios
//                 still fire, but bright peaks contribute their full HDR luma
//                 to the lerp. Use for A/B comparison against Falcor reference.
//                 IMPORTANT: when off, also clear shader_cache/TAA.cs_*.ishdr
//                 so the recompile picks up the new path.
#ifndef TAA_USE_TONEMAP_BLEND
#define TAA_USE_TONEMAP_BLEND 1
#endif

cbuffer TAACB : register(b0, space2)
{
    float4x4 invViewProj;            // inverse(jitteredVP), row-vector convention
    float4x4 prevViewProj;           // previous frame unjittered VP
    uint     width;
    uint     height;
    float    tauHistory;             // diffuse history time constant (seconds)
    float    hasHistory;             // 0 = first frame / after resize, 1 = valid
    float    deltaTime;              // seconds elapsed this frame
    float    jitterX;                // current frame jitter X in pixels [-0.5, +0.5)
    float    jitterY;                // current frame jitter Y in pixels [-0.5, +0.5)
    float    colorBoxSigma;          // base AABB gamma (default 1.5)
    float    colorBoxSigmaSpecular;  // AABB gamma for specular pixels (default 2.0)
    float    specularRoughnessMax;   // roughness threshold for "specular" (default 0.5)
    uint     antiFlicker;            // 0/1 — Falcor distance-to-clamp anti-flicker
    float    velocityWiden;          // motion-proportional AABB widening factor (Fix L)
    float    sharpenStrength;        // Karis 5-tap unsharp blend factor (default 0.1; 0 disables)
    float    _pad0;                  // CB 16-byte alignment
};

// ---- Luma / tonemap ---------------------------------------------------------

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Karis-style reinhard tonemap weighted by max channel (NOT BT.709 luma).
// Why max-channel: BT.709 luma underweights blue (.0722) and overweights green
// (.7152). A saturated emissive like float3(50, 0, 0) has Luma ≈ 10.6 → tonemap
// weight 1/(1+10.6) ≈ 0.087, but a perceptually-similar float3(0, 50, 0) would
// see weight 1/(1+35.8) ≈ 0.027 — i.e. the red firefly gets compressed 3x less
// and leaks into AABB stats / blend. Using max channel makes compression
// channel-uniform: any pure single-channel HDR spike gets the same reduction
// regardless of which channel it lives in. Matches Karis 2014 reference.
float3 ToneMapLuma(float3 c)
{
    return c / (1.0 + max(max(c.r, c.g), c.b));
}

float3 InvToneMapLuma(float3 c)
{
    return c / max(1.0 - max(max(c.r, c.g), c.b), 1e-4);
}

// ---- YCoCg (no-scale) for perceptual neighbourhood clamping -----------------

float3 RGBToYCoCg(float3 c)
{
    return float3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r              - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 YCoCgToRGB(float3 y)
{
    float tmp = y.x - y.z;
    return float3(tmp + y.y, y.x + y.z, tmp - y.y);
}

// ---- Variance AABB clip -----------------------------------------------------
// Project history q onto AABB [mn, mx] along the centre→q ray.
// Component-wise clamp would shorten history toward AABB face but bias along
// the principal channel; ray-based clip preserves chrominance direction.

float3 ClipAABB(float3 mn, float3 mx, float3 q)
{
    float3 center  = 0.5 * (mx + mn);
    float3 extents = 0.5 * (mx - mn) + 1e-5;
    float3 v       = q - center;
    float3 vabs    = abs(v) / extents;
    float  ma      = max(vabs.x, max(vabs.y, vabs.z));
    return (ma > 1.0) ? (center + v / ma) : q;
}

// ---- Screen UV ↔ NDC --------------------------------------------------------

float2 UVToNDC(float2 uv)
{
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

float2 NDCToUV(float2 ndc)
{
    return ndc * float2(0.5, -0.5) + 0.5;
}

#endif // TAA_COMMON_HLSLI
