// DDGICommon.hlsli — shared types + helpers for the DDGI pipeline.
//
// Mirrors C++ struct DDGI::VolumeGPUDesc 1:1. Any field reordering or sizing
// change must be reflected in both files (header and HLSL).

#ifndef DDGI_COMMON_HLSLI
#define DDGI_COMMON_HLSLI

// ---------------------------------------------------------------------------
// Layout constants (also defined as constexpr in DDGIVolumeManager.h).
// ---------------------------------------------------------------------------
#define DDGI_IRRADIANCE_PROBE_SIZE   6
#define DDGI_IRRADIANCE_PROBE_STRIDE (DDGI_IRRADIANCE_PROBE_SIZE + 2)  // +1 border each side
#define DDGI_DEPTH_PROBE_SIZE        16
#define DDGI_DEPTH_PROBE_STRIDE      (DDGI_DEPTH_PROBE_SIZE + 2)
#define DDGI_MAX_VOLUMES             4

// Ray allocation bucketing — matches WickedEngine. raycountBuffer stores
// per-probe bucket counts; actual ray count = buckets * DDGI_RAY_BUCKET_COUNT.
#define DDGI_RAY_BUCKET_COUNT 4

// Probe state values. Encoded so a zero-initialised ProbeData buffer means
// "every probe is active" — no compute-pass init needed. Phase 3
// classification will explicitly write INACTIVE (1) to disable a probe.
#define DDGI_PROBE_STATE_ACTIVE   0
#define DDGI_PROBE_STATE_INACTIVE 1

// ---------------------------------------------------------------------------
// Volume GPU descriptor — must mirror C++ DDGI::VolumeGPUDesc.
// ---------------------------------------------------------------------------
struct DDGIVolumeGPU
{
    float3 origin;        float _padA;
    float3 extent;        float _padB;
    float3 probeSpacing;  float _padC;
    uint   probeCountsX, probeCountsY, probeCountsZ;
    uint   raysPerProbe;
    float  hysteresis;
    float  normalBias;
    float  viewBias;
    float  boundaryFadeRatio;
    uint   flags;            // bit 0 enabled, bit 1 relocation, bit 2 classification, bit 3 overwrite atlas
    uint   _padFlag0;
    uint   _padFlag1;
    uint   _padFlag2;

    float4 randomRotation0;  // float3x3 rows packed in xyz, w unused
    float4 randomRotation1;
    float4 randomRotation2;

    float3 diffuseTint;
    float  diffuseScale;

    // Trace-time lighting integration:
    //   lightCount  — number of valid entries in the cluster GPULight buffer
    //                 bound at t7. 0 → trace shader produces zero direct light
    //                 (only sky miss + multi-bounce contribute).
    //   frameIndex  — per-volume frame counter (multi-bounce branch + RNG seed).
    uint   lightCount;
    uint   frameIndex;
    uint   _padFI0;
    uint   _padFI1;
};

struct DDGIProbeData
{
    float3 offset;
    uint   state;
};

// ---------------------------------------------------------------------------
// Probe coordinate <-> linear index <-> texture-atlas position.
// ---------------------------------------------------------------------------
uint DDGI_ProbeIndex(int3 coord, DDGIVolumeGPU vol)
{
    return (uint)coord.x
         + (uint)coord.y * vol.probeCountsX
         + (uint)coord.z * vol.probeCountsX * vol.probeCountsY;
}

int3 DDGI_ProbeCoord(uint idx, DDGIVolumeGPU vol)
{
    int z = (int)(idx / (vol.probeCountsX * vol.probeCountsY));
    int rem = (int)(idx % (vol.probeCountsX * vol.probeCountsY));
    int y = rem / (int)vol.probeCountsX;
    int x = rem % (int)vol.probeCountsX;
    return int3(x, y, z);
}

// World-space position of a probe (without relocation offset).
float3 DDGI_ProbeWorldPos(int3 coord, DDGIVolumeGPU vol)
{
    // Grid runs from origin - extent .. origin + extent. With probeCountsN
    // probes along an axis there are (probeCountsN - 1) cells; first probe
    // sits at the negative boundary.
    float3 t = float3(coord) / max(float3(vol.probeCountsX - 1,
                                          vol.probeCountsY - 1,
                                          vol.probeCountsZ - 1), 1.0);
    return vol.origin - vol.extent + t * (vol.extent * 2.0);
}

// Atlas tile origin (in texels) for the (probe, atlas-stride) tuple.
// Atlas layout: probeCountX × (probeCountY * probeCountZ) tiles.
uint2 DDGI_ProbeAtlasTile(int3 coord, DDGIVolumeGPU vol, uint stride)
{
    uint x = (uint)coord.x * stride;
    uint y = ((uint)coord.y + (uint)coord.z * vol.probeCountsY) * stride;
    return uint2(x, y);
}

// ---------------------------------------------------------------------------
// Octahedral mapping (Cigolle 2014).
// ---------------------------------------------------------------------------
float2 DDGI_SignNotZero(float2 v) { return float2(v.x >= 0.0 ? 1.0 : -1.0,
                                                  v.y >= 0.0 ? 1.0 : -1.0); }

// Map a unit vector to [-1,1]^2.
float2 DDGI_OctEncode(float3 n)
{
    float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    return n.z >= 0.0 ? p : (1.0 - abs(p.yx)) * DDGI_SignNotZero(p);
}

// Inverse: map [-1,1]^2 → unit vector.
float3 DDGI_OctDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * DDGI_SignNotZero(n.xy);
    return normalize(n);
}

// Map a probe-local octahedral texel coordinate (in [0..probeSize-1]) to a
// unit direction. Borders excluded — caller handles them.
float3 DDGI_DirectionFromTexel(uint2 texel, uint probeSize)
{
    // Map from integer texel center to [-1,1] octahedron.
    float2 uv = (float2(texel) + 0.5) / float(probeSize);
    return DDGI_OctDecode(uv * 2.0 - 1.0);
}

// ---------------------------------------------------------------------------
// Volume → grid-space mapping (used by DDGISampling.hlsli at shading time).
// ---------------------------------------------------------------------------
float3 DDGI_GridSpace(float3 worldPos, DDGIVolumeGPU vol)
{
    float3 minCorner = vol.origin - vol.extent;
    return (worldPos - minCorner) / vol.probeSpacing;
}

// Volume membership / fade weight in [0..1]. 1 inside; falls off near AABB
// boundary by `boundaryFadeRatio`.
float DDGI_VolumeFadeWeight(float3 worldPos, DDGIVolumeGPU vol)
{
    float3 d = abs(worldPos - vol.origin) / max(vol.extent, 1e-3);
    float maxd = max(d.x, max(d.y, d.z));
    if (maxd >= 1.0) return 0.0;
    float fadeStart = 1.0 - vol.boundaryFadeRatio;
    return saturate(1.0 - max(maxd - fadeStart, 0.0) / max(vol.boundaryFadeRatio, 1e-4));
}

// Apply volume-space random rotation to a unit vector. Used by the ray
// generation step so per-frame ray orientations spread uniformly.
float3 DDGI_ApplyRandomRotation(float3 dir, DDGIVolumeGPU vol)
{
    return float3(
        dot(vol.randomRotation0.xyz, dir),
        dot(vol.randomRotation1.xyz, dir),
        dot(vol.randomRotation2.xyz, dir));
}

// ---------------------------------------------------------------------------
// Halton(2, 3) on the unit sphere — uniform-on-sphere directions for ray
// generation.
//
// Replaces the spherical-Fibonacci sequence this engine used previously.
// Fibonacci is sorted by latitude (cosTh = 1 - (2i+1)/count is monotonically
// decreasing in i), so any prefix [0..k) is a polar cap, NOT a uniform
// subset of the sphere. Adaptive ray dispatch — which iterates only the
// first `adaptiveRays` indices — was therefore sampling only the "north"
// hemisphere of every probe and producing strong axis-aligned bias in the
// SH (one wall lit, opposite wall dark, even under perfectly vertical sun).
//
// Halton has the prefix-uniform property by construction: each new sample
// fills the largest gap in the existing distribution, so any prefix is
// approximately uniform on the unit sphere.
//
// Side benefit: cosTh no longer depends on a `count` parameter. Adaptive
// dispatch can use the same direction at index `i` regardless of how many
// rays this frame's probe traces — the "must pass raysPerProbe (max) into
// Fibonacci to keep directions stable across frames" workaround is gone.
// ---------------------------------------------------------------------------
#ifndef DDGI_PI
#define DDGI_PI 3.14159265359
#endif

float DDGI_RadicalInverseBase2(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 2^32
}

float DDGI_RadicalInverseBase3(uint i)
{
    float r = 0.0;
    float f = 1.0 / 3.0;
    // 20 digits covers i up to 3^20 ≈ 3.5 billion — far past any plausible
    // probe ray count. Loop is bounded so HLSL can unroll cleanly.
    [unroll(20)]
    for (uint k = 0u; k < 20u; ++k)
    {
        if (i == 0u) break;
        r += f * float(i % 3u);
        i /= 3u;
        f *= 1.0 / 3.0;
    }
    return r;
}

float3 DDGI_HaltonSphere(uint i)
{
    // i+1 skips the singularity at the exact pole (cosTh=1 makes phi
    // unobservable). The shift doesn't affect distribution properties.
    const uint  k     = i + 1u;
    const float u1    = DDGI_RadicalInverseBase2(k);
    const float u2    = DDGI_RadicalInverseBase3(k);
    const float cosTh = 1.0 - 2.0 * u1;                  // uniform [-1, 1]
    const float sinTh = sqrt(saturate(1.0 - cosTh * cosTh));
    const float phi   = 2.0 * DDGI_PI * u2;
    return float3(cos(phi) * sinTh, sin(phi) * sinTh, cosTh);
}

// Per-frame adaptive sample index. Each frame samples a CONTIGUOUS block of
// `adaptiveRays` Halton indices, advancing `adaptiveRays` per frame, so that
// over `raysPerProbe / adaptiveRays` frames every position in the full
// sequence is sampled exactly once.
//
// Why: with adaptive ray count = N < raysPerProbe, naively taking Halton
// indices [0..N-1] every frame samples the SAME N world-space directions
// forever (the ±17° random rotation jitter is smaller than Halton's inter-
// sample arc at low ray count). The remaining ~89% of the sphere never
// enters the SH, producing a constant per-probe directional bias visible
// as "one wall always bright, opposite wall always dark", independent of
// sun direction.
//
// Why CONTIGUOUS block, not stride: an earlier attempt used stride-K
// sampling (rayIdx*stride + frameOffset). That's broken for base-2 Halton
// because stride=4 selects k = 1, 5, 9, 13, ... (all odd integers); odd k
// has its LSB=1, which after the 32-bit bit reverse becomes the MSB → u1
// ≥ 0.5 for ALL strided samples → cosTh ≤ 0 for ALL → only the LOWER
// hemisphere is sampled. Contiguous Halton ranges keep the prefix-uniform
// property: vdC2 maps [0..2^n) → multiples of 1/2^n, so any contiguous
// range of size 2^k still tiles [0,1] uniformly.
uint DDGI_AdaptiveSampleIdx(uint rayIdx, uint adaptiveRays, DDGIVolumeGPU vol)
{
    const uint adaptive   = max(adaptiveRays, 1u);
    const uint baseOffset = (vol.frameIndex * adaptive) % vol.raysPerProbe;
    return (rayIdx + baseOffset) % vol.raysPerProbe;
}

// ---------------------------------------------------------------------------
// Variance / multi-scale mean estimator (port of WickedEngine ShaderInterop_DDGI.h).
// Per-output-texel running stats consumed by DDGIRelight.cs to suppress
// fireflies and switch between fast/slow blends adaptively. Replaces the plain
// EMA hysteresis lerp.
//
// Storage: one DDGIVarianceData per (probe, irradiance-tile texel). Sized
// 48 B so the StructuredBuffer can be allocated from float math without packing.
// ---------------------------------------------------------------------------
struct DDGIVarianceData
{
    float3 mean;          float vbbr;
    float3 shortMean;     float inconsistency;
    float3 variance;      float _pad;
};

// ---------------------------------------------------------------------------
// L1 spherical harmonics — replaces the per-probe octahedral irradiance atlas.
// One DDGIProbeSH per probe (48 B): 4 RGB SH coefficients projected from the
// hemisphere of traced rays. Sampling reduces to a single dot product per
// channel, no bilinear texture lookup, no octahedral border.
// ---------------------------------------------------------------------------
struct DDGIProbeSH
{
    // .w = L_{0,0}, .xyz = L_{1,-1} L_{1,0} L_{1,+1} (mapped to N.y, N.z, N.x).
    float4 R;
    float4 G;
    float4 B;
};

DDGIProbeSH DDGI_SH_Zero()
{
    DDGIProbeSH sh;
    sh.R = float4(0, 0, 0, 0);
    sh.G = float4(0, 0, 0, 0);
    sh.B = float4(0, 0, 0, 0);
    return sh;
}

// Project one radiance sample (`color` along `dir`) onto L1 SH. Caller
// accumulates and normalises by 4π / N_samples after the loop.
DDGIProbeSH DDGI_SH_Project(float3 dir, float3 color)
{
    const float Y00 = 0.282094792;
    const float Y1  = 0.488602512;
    DDGIProbeSH sh;
    float3 nyx = float3(dir.y, dir.z, dir.x);
    sh.R.w   = color.r * Y00; sh.R.xyz = color.r * Y1 * nyx;
    sh.G.w   = color.g * Y00; sh.G.xyz = color.g * Y1 * nyx;
    sh.B.w   = color.b * Y00; sh.B.xyz = color.b * Y1 * nyx;
    return sh;
}

DDGIProbeSH DDGI_SH_Add(DDGIProbeSH a, DDGIProbeSH b)
{
    DDGIProbeSH r;
    r.R = a.R + b.R; r.G = a.G + b.G; r.B = a.B + b.B;
    return r;
}

DDGIProbeSH DDGI_SH_Lerp(DDGIProbeSH a, DDGIProbeSH b, float t)
{
    DDGIProbeSH r;
    r.R = lerp(a.R, b.R, t);
    r.G = lerp(a.G, b.G, t);
    r.B = lerp(a.B, b.B, t);
    return r;
}

DDGIProbeSH DDGI_SH_Multiply(DDGIProbeSH a, float s)
{
    DDGIProbeSH r;
    r.R = a.R * s; r.G = a.G * s; r.B = a.B * s;
    return r;
}

// Reconstruct the diffuse-IBL signal from the L1 SH projected by
// DDGIRelight.cs. Output magnitude is calibrated to match the engine's
// existing sky-IBL convention (sky_sh.hlsli's EvalSH2 — radiance projected
// onto SH, reconstructed without cosine-lobe convolution, then used
// directly as `iblDiffuse * albedo` in Lighting.ps).
//
// Coefficient choice:
//
//   c0 = π · Y00 ≈ 0.886    →  L0 reconstruction matches the magnitude
//                              the previous (bug-for-bug) implementation
//                              produced, and matches what the lighting
//                              consumer was calibrated against. Choosing
//                              the strictly-physical Y00 = 0.282 instead
//                              divides the result by π, making every probe
//                              read invisible against the same-magnitude
//                              sky-IBL path (observed regression: "probes
//                              appear to have no effect").
//
//   c1 = π · Y1 ≈ 1.534     →  L1 weight relative to L0 stays at the
//                              physical Y1/Y00 = √3 ratio. The previous
//                              code used (2π/3)·Y1 ≈ 1.023, a Ramamoorthi
//                              clamped-cosine convolution constant whose
//                              c1/c0 ratio of ≈1.155 squashed L1 by 2/3 —
//                              probes were still over-bright but felt
//                              omnidirectional. Using π·Y1 keeps the
//                              brightness AND restores directionality.
//
// This is a calibration choice, not a derivation: the projection in
// DDGIRelight stores `cos-weighted radiance average ≈ E(D)/π` and the
// engine's diffuse IBL consumer multiplies by albedo without dividing by
// π, so the probe path needs the extra π factor to land on the same
// dimensionally-mixed-but-internally-consistent scale as the sky path.
//
// NaN/Inf gate: max(r, 0.0) does NOT strip NaN (IEEE 754 specifies
// max(NaN, x) = NaN) — a single corrupt SH coefficient propagating into
// Lighting.ps and re-entering the multi-bounce feedback loop in
// DDGIRayTrace would poison probes every frame ("neon-green/magenta
// spheres that ignore intensity scaling"). Quarantining at the read site
// breaks the loop deterministically.
float3 DDGI_SH_Irradiance(DDGIProbeSH sh, float3 N)
{
    const float c0 = 0.886226926; // π · Y00
    const float c1 = 1.534990081; // π · Y1   (Y1/Y00 ≈ √3 ratio for full directionality)
    float3 nyx = float3(N.y, N.z, N.x);

    // L0 and per-channel L1·N decoded separately so we can clamp each
    // channel's L1 contribution before the final sum.
    float L0r = sh.R.w, L0g = sh.G.w, L0b = sh.B.w;
    float L1r = dot(nyx, sh.R.xyz);
    float L1g = dot(nyx, sh.G.xyz);
    float L1b = dot(nyx, sh.B.xyz);

    // ---- Negative-lobe protection ------------------------------------------
    // Naive `max(r, 0)` clips the FINAL per-channel result to zero whenever
    // c1·L1·N exceeds -c0·L0. Because the L1 magnitude can differ per
    // channel (in this engine it does — we observed the geometric-series
    // multi-bounce loop driving sh.G near zero while sh.R / sh.B amplified),
    // the clip fires asymmetrically and locks the probe onto a magenta-ish
    // (R≈B big, G≈0) steady state. The standard production fix is to bound
    // L1's negative excursion per channel relative to its OWN L0, so each
    // channel keeps a small positive floor and the channel-asymmetric clip
    // can no longer happen.
    //
    // We choose floor=0.05 (reconstructed channel ≥ 5% of pure-L0 result),
    // which corresponds to L1·N ≥ -0.95·(c0/c1)·L0 ≈ -0.548·L0. Channels
    // with strongly-negative L1·N still get heavily attenuated (preserving
    // the directional cue) but never collapse to zero.
    const float kAlpha = 0.548f; // 0.95 * c0 / c1
    L1r = max(L1r, -kAlpha * max(L0r, 0.0));
    L1g = max(L1g, -kAlpha * max(L0g, 0.0));
    L1b = max(L1b, -kAlpha * max(L0b, 0.0));

    float3 r;
    r.r = c0 * L0r + c1 * L1r;
    r.g = c0 * L0g + c1 * L1g;
    r.b = c0 * L0b + c1 * L1b;

    if (any(isnan(r)) || any(isinf(r))) return float3(0, 0, 0);
    return max(r, 0.0);
}

void MultiscaleMeanEstimator(float3 y, inout DDGIVarianceData data, float shortWindowBlend)
{
    float3 mean      = data.mean;
    float3 shortMean = data.shortMean;
    float  vbbr      = data.vbbr;
    float3 variance  = data.variance;
    float  inconsistency = data.inconsistency;

    // Suppress fireflies before they enter the integration.
    {
        float3 dev = sqrt(max(1e-5, variance));
        float3 highThreshold = 0.1 + shortMean + dev * 8.0;
        float3 overflow = max(0, y - highThreshold);
        y -= overflow;
    }

    float3 delta = y - shortMean;
    shortMean    = lerp(shortMean, y, shortWindowBlend);
    float3 delta2 = y - shortMean;

    float varianceBlend = shortWindowBlend * 0.5;
    variance = lerp(variance, delta * delta2, varianceBlend);
    float3 dev = sqrt(max(1e-5, variance));

    float3 shortDiff = mean - shortMean;
    float relativeDiff = dot(float3(0.299, 0.587, 0.114), abs(shortDiff) / max(1e-5, dev));
    inconsistency = lerp(inconsistency, relativeDiff, 0.08);

    float varianceBasedBlendReduction =
        clamp(dot(float3(0.299, 0.587, 0.114), 0.5 * shortMean / max(1e-5, dev)), 1.0/32.0, 1.0);

    float3 catchUpBlend = clamp(smoothstep(0.0, 1.0,
        relativeDiff * max(0.02, inconsistency - 0.2)), 1.0/256.0, 1.0);
    catchUpBlend *= vbbr;

    vbbr = lerp(vbbr, varianceBasedBlendReduction, 0.1);
    mean = lerp(mean, y, saturate(catchUpBlend));

    data.mean          = mean;
    data.shortMean     = shortMean;
    data.vbbr          = vbbr;
    data.variance      = variance;
    data.inconsistency = inconsistency;
}

#endif // DDGI_COMMON_HLSLI
