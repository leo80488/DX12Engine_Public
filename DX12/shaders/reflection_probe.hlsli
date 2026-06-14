#ifndef REFLECTION_PROBE_HLSLI
#define REFLECTION_PROBE_HLSLI

// reflection_probe.hlsli — shared probe struct, persistent SRVs, and the two
// math helpers (parallax-correct AABB + influence falloff) used wherever
// reflection probes are evaluated.
//
// The cube-array + metadata buffer are persistent SRVs allocated by
// ReflectionProbeManager and bound at root-sig slots 37/38 (t23/t24 space0)
// from both LightingPass and TransparentPass. Each consumer drives the
// per-pixel probe-loop locally because the loop's shape differs (deferred
// uses the cluster grid, forward iterates all probes), but the data layout
// and the per-probe math are common — that is what lives here.
//
// Cluster-list SRVs (gReflectionProbeGrid / gReflectionProbeIndex at t25 / t26)
// are NOT declared here on purpose; only LightingPass uses them, so they stay
// in Lighting.ps.hlsl to avoid forcing TransparentPass to bind a slot it
// doesn't read.
//
// Probe struct layout MIRRORS the C++ side — see
// include/Graphics/ReflectionProbeTypes.h (Reflection::GPUReflectionProbe,
// 64 bytes). Edit all three copies together (here + ClusterCullProbes.cs.hlsl
// + the C++ struct).

struct ReflectionProbe
{
    float3 position;        float influenceRadius;
    float3 boxMin;          uint   cubemapSlice;
    float3 boxMax;          uint   flags;
    // intensity = per-probe radiance multiplier, independent of iblStrength.
    float3 innerExtents;    float  intensity;
};

TextureCubeArray<float4>          gReflectionProbeArray : register(t23, space0);
StructuredBuffer<ReflectionProbe> gReflectionProbes     : register(t24, space0);

// Parallax-correct a reflection direction R against the probe's AABB so the
// sampled direction points at the actual world-space hit on the box, not the
// infinite "as if on a sphere at infinity" point. Standard box projection
// (Lazarov / UE4): trace R from worldPos, find the smallest positive t to
// any face, re-project from probe centre to the hit. Returned vector is NOT
// normalised — TextureCube samplers handle that.
float3 ParallaxCorrectAABB(float3 R, float3 worldPos, float3 probePos,
                           float3 boxMin, float3 boxMax)
{
    // Component-wise t to each face. R near zero → division produces +inf,
    // which max/min collapse out as long as at least one axis is finite.
    float3 invR      = 1.0 / R;
    float3 toMaxFace = (boxMax - worldPos) * invR;
    float3 toMinFace = (boxMin - worldPos) * invR;
    float3 furthest  = max(toMaxFace, toMinFace);
    float  dist      = min(min(furthest.x, furthest.y), furthest.z);
    float3 hitPos    = worldPos + R * dist;
    return hitPos - probePos;
}

// Inner / outer box weighting. Inside inner → 1.0 (full influence). Between
// inner and outer → linear fade per axis (min across axes wins so any axis
// fully outside drops the weight to 0). Outside outer → 0.0.
//
// innerExtents and the OUTER half-extents (derived from boxMin/boxMax) are
// independent artist-set knobs; the CPU upload guarantees inner ≤ outer per
// axis so falloffWidth is always non-negative.
float ComputeProbeWeight(float3 worldPos, ReflectionProbe probe)
{
    float3 probeCenter = 0.5 * (probe.boxMin + probe.boxMax);
    float3 outerHalf   = 0.5 * (probe.boxMax - probe.boxMin);
    float3 innerHalf   = probe.innerExtents;
    float3 local       = abs(worldPos - probeCenter);

    // Outside the outer box on any axis → no influence.
    if (any(local > outerHalf)) return 0.0;

    // Per-axis fade from 1 at innerHalf to 0 at outerHalf. saturate handles
    // local < innerHalf (negative numerator) by clamping to 1.
    float3 falloffWidth = max(outerHalf - innerHalf, 1e-4);
    float3 axisWeight   = saturate((outerHalf - local) / falloffWidth);

    // Min across axes — the most constraining axis dominates, matches UE box
    // projection conventions and avoids "edge bleed" at corners.
    return min(min(axisWeight.x, axisWeight.y), axisWeight.z);
}

#endif // REFLECTION_PROBE_HLSLI
