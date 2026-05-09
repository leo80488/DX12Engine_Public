// DDGIPrepareRayCount.cs.hlsl — adaptive per-probe ray count.
//
// Reads the variance buffer (from last frame's relight) and decides how many
// ray buckets each probe needs this frame. Probes whose multi-scale mean
// estimator is converged (low inconsistency, low variance) get fewer rays;
// probes with high inconsistency get the full budget.
//
// Output: g_RayCount[probeIdx] = bucket count in [minBuckets..maxBuckets].
// Actual ray count = bucketCount * DDGI_RAY_BUCKET_COUNT.
//
// Also resets g_RayAlloc[3] (total ray counter) so the allocation CS can
// InterlockedAdd into it.

#include "DDGICommon.hlsli"

ConstantBuffer<DDGIVolumeGPU>          g_Vol      : register(b0, space0);
RWStructuredBuffer<DDGIVarianceData>   g_Variance : register(u1, space0);
RWStructuredBuffer<uint>               g_RayCount : register(u2, space0);
RWStructuredBuffer<uint>               g_RayAlloc : register(u3, space0);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint probeCount = g_Vol.probeCountsX * g_Vol.probeCountsY * g_Vol.probeCountsZ;
    const uint probeIdx = DTid.x;

    // First thread of dispatch resets the running atomic counter for the
    // allocation pass that follows.
    if (probeIdx == 0)
        g_RayAlloc[0] = 0;

    if (probeIdx >= probeCount) return;

    const uint maxBuckets = max(g_Vol.raysPerProbe / DDGI_RAY_BUCKET_COUNT, 1u);
    const uint minBuckets = max(maxBuckets / 4u, 1u);

    // Variance-driven adaptive ray count. We pick the largest "inconsistency"
    // signal across this probe's irradiance-tile texels — a probe whose
    // estimator has converged (low inconsistency, low variance/mean ratio)
    // gets the minimum budget; any texel that's still moving forces the
    // probe to the full budget. Sky-converged probes drop to ~16 rays
    // immediately after warm-up, which roughly halves the trace cost in
    // steady-state scenes — the win that pushes large grids back under TDR.
    //
    // Override paths:
    //  * frameIndex < 8           — burn-in: every probe full budget.
    //  * flags & 8u (overwrite)   — first-allocation frames: full budget.
    //  * flags & 1u == 0          — slot inactive: zero rays (Tick zeros
    //                               the desc; this is just defensive).
    if ((g_Vol.flags & 1u) == 0u) { g_RayCount[probeIdx] = 0u; return; }
    if (g_Vol.frameIndex < 8u || (g_Vol.flags & 8u) != 0u)
    {
        g_RayCount[probeIdx] = maxBuckets;
        return;
    }

    const uint kProbeSize = DDGI_IRRADIANCE_PROBE_SIZE;
    const uint vBase      = probeIdx * kProbeSize * kProbeSize;
    float maxInconsistency = 0.0;
    [loop] for (uint texel = 0; texel < kProbeSize * kProbeSize; ++texel)
    {
        DDGIVarianceData vd = g_Variance[vBase + texel];
        // NaN-safe: lerp(NaN,…) = NaN; treat as fully-inconsistent so the
        // probe gets a full budget that resets the texel within ~30 frames.
        float inc = (isnan(vd.inconsistency) || isinf(vd.inconsistency))
                        ? 1.0 : vd.inconsistency;
        maxInconsistency = max(maxInconsistency, inc);
    }

    // Threshold from WickedEngine: inconsistency < 0.2 means "noise-level
    // variation, no scene change to track". Linearly ramp the bucket count
    // from min..max as inconsistency rises across [0.2, 1.0].
    const float incRamp = saturate((maxInconsistency - 0.2) / 0.8);
    const uint  buckets = (uint)round(lerp(float(minBuckets), float(maxBuckets), incRamp));
    g_RayCount[probeIdx] = clamp(buckets, minBuckets, maxBuckets);
}
