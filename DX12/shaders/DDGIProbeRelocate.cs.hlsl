// DDGIProbeRelocate.cs.hlsl — push probes out of nearby surfaces.
//
// Each probe runs `raysPerProbe` rays (in the trace CS). If many of those
// rays hit at very short distance, the probe is either inside a wall or
// pressed against one — the resulting bounce data has wildly wrong
// magnitude (close-range bounces look like point lights), which feeds back
// through DDGI multi-bounce and produces "blown-out" indirect lighting.
//
// Algorithm (matches WickedEngine ddgi_updateCS_depth.hlsl probe-offset path):
//   1. Per probe, scan all this-frame rays.
//   2. For each ray with depth < keepDistance, accumulate
//      `-rayDir * (keepDistance - depth)` into the offset target.
//   3. EMA-blend the per-probe ProbeData.offset toward this target (slow
//      blend = 5%/frame) so probe positions don't jitter on noisy hits.
//   4. Clamp the offset to ±0.45 × cellSize to keep the probe inside its
//      authored cell (preserves the tri-linear blend math at sample time).
//
// Sampling already reads `probePos + pd.offset` in DDGISampling.hlsli, so
// the shader-side change is only this CS — Lighting.ps and the trace's
// multi-bounce path pick up the new positions automatically next frame.
//
// Bindings:
//   b0 space0 — DDGIVolumeGPU CB
//   u0 space0 — RWStructuredBuffer<DDGIProbeData> (write back offset)
//   t2 space0 — StructuredBuffer<float4> ray data (read depth from .w)
//   u2 space0 — RWStructuredBuffer<uint>  ray count (per-probe adaptive)

#include "DDGICommon.hlsli"

ConstantBuffer<DDGIVolumeGPU>      g_Vol      : register(b0, space0);
RWStructuredBuffer<DDGIProbeData>  g_ProbeData : register(u0, space0);
StructuredBuffer<float4>           g_RayData  : register(t2, space0);
RWStructuredBuffer<uint>           g_RayCount : register(u2, space0);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint probeIdx   = DTid.x;
    const uint probeCount = g_Vol.probeCountsX * g_Vol.probeCountsY * g_Vol.probeCountsZ;
    if (probeIdx >= probeCount) return;

    // Bit 1 of flags = enableRelocation (mirror of DDGIVolumeComponent toggle).
    if ((g_Vol.flags & 2u) == 0u) return;

    // Adaptive ray prefix; direction reconstruction uses Halton(2,3) which
    // is index-stable independent of count (matches trace + relight). Ray
    // data buffer stride is the volume's MAX raysPerProbe.
    const uint adaptiveRays = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;
    const uint rayBase      = probeIdx * g_Vol.raysPerProbe;

    // Probes should keep at least this distance from any surface. Scaled by
    // the smallest cell dimension so the threshold tracks the volume's
    // resolution — finer grids → smaller threshold, coarser → larger.
    const float minCell      = min(g_Vol.probeSpacing.x,
                                   min(g_Vol.probeSpacing.y, g_Vol.probeSpacing.z));
    const float keepDistance = minCell * 0.25;

    // Accumulate "push-away" pressure from every too-close hit.
    float3 offsetAccum = float3(0, 0, 0);
    uint   closeHits   = 0;

    [loop] for (uint r = 0; r < adaptiveRays; ++r)
    {
        const float depth = g_RayData[rayBase + r].w;
        if (depth <= 0.0 || depth >= keepDistance) continue;

        // Reconstruct the world-space ray direction. Must mirror the trace +
        // relight reconstruction exactly (same adaptive sample index + random
        // rotation) or the offset push goes the wrong way.
        const uint sampleIdx = DDGI_AdaptiveSampleIdx(r, adaptiveRays, g_Vol);
        const float3 dirLocal = DDGI_HaltonSphere(sampleIdx);
        const float3 rayDir   = DDGI_ApplyRandomRotation(dirLocal, g_Vol);

        // Push the probe AWAY from the hit (-rayDir), proportional to how
        // close it was. A ray hitting at depth=0 contributes a full
        // keepDistance push.
        offsetAccum -= rayDir * (keepDistance - depth);
        closeHits++;
    }

    DDGIProbeData pd = g_ProbeData[probeIdx];

    // No close hits this frame → the current offset is already good (probe
    // is comfortably inside open space). Hold it in place. Resetting to zero
    // here would tug the probe back toward the grid every "lucky" frame and
    // produce visible jitter on probes that sit near walls but not inside
    // them, oscillating between "many close hits → push outward" and "no
    // close hits → snap back to origin".
    if (closeHits == 0u)
    {
        // Still write to keep state field intact for callers that read it.
        g_ProbeData[probeIdx] = pd;
        return;
    }

    // AVERAGE the per-ray push (the previous code accumulated unscaled, so
    // the target magnitude scaled with hit count and saturated the clamp
    // immediately on probes that had many close hits). With averaging the
    // target stays in [0, keepDistance] regardless of how many rays hit.
    const float3 targetOffset = offsetAccum / float(closeHits);

    // Slow EMA blend; reduced from 0.05 to 0.02 because per-frame target
    // jitter (different rays each frame after random-rotation damping)
    // remained noticeable at 0.05 once probes settled near a wall. 0.02
    // gives ~50-frame settle time which is invisible at 60 fps.
    float3 newOffset = lerp(pd.offset, targetOffset, 0.02);

    // Clamp to the authored cell — going further would put the probe inside
    // a neighbouring cell and break the trilinear blend at sample time.
    const float3 limit = g_Vol.probeSpacing * 0.45;
    newOffset = clamp(newOffset, -limit, limit);

    pd.offset = newOffset;
    g_ProbeData[probeIdx] = pd;
}
