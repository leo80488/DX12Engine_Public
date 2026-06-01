// DDGIProbeRelocate.cs.hlsl — push probes out of nearby surfaces + classify
// probes that are buried in solid geometry as INACTIVE.
//
// Two cooperating mechanisms in one CS, both reading the per-probe ray buffer:
//
//   (A) Frontface close-hit push (the original WickedEngine ddgi_updateCS
//       probe-offset path):
//         1. Per probe, scan all this-frame rays.
//         2. For each ray with depth > 0 and depth < keepDistance, accumulate
//            `-rayDir * (keepDistance - depth)` into a frontface push target.
//         3. EMA-blend the per-probe ProbeData.offset toward this target so
//            probe positions don't jitter on noisy hits.
//         4. Clamp the offset to ±0.45 × cellSize to keep the probe inside
//            its authored cell (preserves the tri-linear blend math at sample
//            time).
//
//   (B) Backface-ratio probe classification + escape push (this engine's
//       indoor↔outdoor light-leak fix, 2026-05):
//         Trace shader writes hitDistance < -1.5 when its closest hit is on a
//         BACKFACE — meaning the probe sits on the SOLID side of that surface
//         along that ray direction.
//         - If > 25% of the probe's rays hit backfaces, the probe is largely
//           buried; flag it INACTIVE (sampler skips inactive probes and the
//           trilinear weight redistributes to neighbours).
//         - Even before INACTIVE kicks in, push the probe OPPOSITE the mean
//           backface ray direction — pulls partially-buried probes toward
//           whichever side is open. This is a different signal from (A): (A)
//           pushes from NEAR FRONTFACES, (B) pulls TOWARD OPEN SPACE based on
//           which directions don't see solid material.
//         Hysteresis: > 25% backface flags INACTIVE, < 10% reactivates. Dead
//         band in between keeps the state from flickering on noise.
//
// Hit-distance encoding (set in DDGIRayTrace.cs):
//   depth > 0       — frontface hit at this distance
//   depth == -1.0   — miss (no hit, sky)
//   depth < -1.5    — backface hit, real t = -(depth + 2.0)
//
// Sampling already reads `probePos + pd.offset` and skips state==INACTIVE in
// DDGISampling.hlsli, so the shader-side change is only this CS — Lighting.ps
// and the trace's multi-bounce path pick up the new positions/states next frame.
//
// Bindings:
//   b0 space0 — DDGIVolumeGPU CB
//   u0 space0 — RWStructuredBuffer<DDGIProbeData> (write back offset + state)
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

    // Flag bits (mirror of DDGIVolumeComponent toggles via DDGIVolumeManager):
    //   bit 1 — enableRelocation
    //   bit 2 — enableClassification
    const bool doRelocate = (g_Vol.flags & 2u) != 0u;
    const bool doClassify = (g_Vol.flags & 4u) != 0u;
    if (!doRelocate && !doClassify) return;

    // Adaptive ray prefix; direction reconstruction uses Halton(2,3) which
    // is index-stable independent of count (matches trace + relight). Ray
    // data buffer stride is the volume's MAX raysPerProbe.
    const uint adaptiveRays = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;
    const uint rayBase      = probeIdx * g_Vol.raysPerProbe;
    if (adaptiveRays == 0u)
    {
        // No rays this frame — nothing to learn; leave probe state intact.
        return;
    }

    // Probes should keep at least this distance from any surface. Scaled by
    // the smallest cell dimension so the threshold tracks the volume's
    // resolution — finer grids → smaller threshold, coarser → larger.
    const float minCell      = min(g_Vol.probeSpacing.x,
                                   min(g_Vol.probeSpacing.y, g_Vol.probeSpacing.z));
    const float keepDistance = minCell * 0.25;

    // Per-probe ray-scan totals.
    float3 offsetAccum     = float3(0, 0, 0);  // (A) close-frontface push
    uint   closeHits       = 0;
    float3 backfaceDirAcc  = float3(0, 0, 0);  // (B) mean backface direction
    uint   backfaceHits    = 0;

    [loop] for (uint r = 0; r < adaptiveRays; ++r)
    {
        const float depth = g_RayData[rayBase + r].w;

        // Reconstruct the world-space ray direction. Must mirror the trace +
        // relight reconstruction exactly (same adaptive sample index + random
        // rotation) or the offset push goes the wrong way.
        const uint sampleIdx = DDGI_AdaptiveSampleIdx(r, adaptiveRays, g_Vol);
        const float3 dirLocal = DDGI_HaltonSphere(sampleIdx);
        const float3 rayDir   = DDGI_ApplyRandomRotation(dirLocal, g_Vol);

        // Backface hit — probe is on the solid side along this direction.
        // Don't add to the close-hit push (the actual t may be larger than
        // keepDistance, but the geometry is still "wrapping" the probe).
        if (depth < -1.5)
        {
            backfaceHits++;
            backfaceDirAcc += rayDir;
            continue;
        }

        // Miss or far frontface — no relocation pressure either way.
        if (depth <= 0.0 || depth >= keepDistance) continue;

        // Close frontface hit — push AWAY from it, proportional to how close.
        offsetAccum -= rayDir * (keepDistance - depth);
        closeHits++;
    }

    DDGIProbeData pd = g_ProbeData[probeIdx];

    // ---- Classification (B-1) ---------------------------------------------
    // Hysteresis band: > 25% backface marks INACTIVE; < 10% reactivates. The
    // 15 pp dead-band swallows per-frame noise so partially-buried probes
    // sitting near the threshold don't flicker state.
    if (doClassify)
    {
        if (backfaceHits * 4u > adaptiveRays)
            pd.state = DDGI_PROBE_STATE_INACTIVE;
        else if (backfaceHits * 10u < adaptiveRays)
            pd.state = DDGI_PROBE_STATE_ACTIVE;
        // else: dead-band — keep current state.
    }

    // ---- Relocation (A + B-2) ---------------------------------------------
    if (doRelocate)
    {
        const bool anySignal = (closeHits > 0u) || (backfaceHits > 0u);
        if (!anySignal)
        {
            // Probe sits in open space with nothing near it — hold the current
            // offset (zeroing here would tug it back to the grid every "lucky"
            // frame and produce visible jitter on probes that sit near walls
            // but not inside them, oscillating between "many close hits" and
            // "no close hits" frames).
            g_ProbeData[probeIdx] = pd;
            return;
        }

        float3 targetOffset = float3(0, 0, 0);

        // (A) Average the per-ray frontface push so target magnitude stays in
        // [0, keepDistance] regardless of how many rays hit (the pre-fix code
        // accumulated unscaled and saturated the ±0.45-cell clamp on probes
        // with many close hits).
        if (closeHits > 0u)
            targetOffset += offsetAccum / float(closeHits);

        // (B-2) Backface escape push — opposite of the MEAN backface ray
        // direction. A probe buried in a wall has rays from many directions
        // hitting backfaces; their mean roughly points toward whichever side
        // of the geometry is closest to "open." Pushing OPPOSITE pulls the
        // probe through the wall toward the open side. Magnitude scaled by
        // backface fraction (× 2 because the cell clamp will catch overshoot
        // and we'd rather fully escape than under-shoot).
        if (backfaceHits > 0u)
        {
            const float3 meanBack = backfaceDirAcc / float(backfaceHits);
            const float  bfFrac   = float(backfaceHits) / float(adaptiveRays);
            targetOffset += -meanBack * keepDistance * bfFrac * 2.0;
        }

        // Slow EMA blend; 0.02/frame ≈ 50-frame settle, invisible at 60 fps
        // and quiet enough that noise on the target doesn't shake settled
        // probes.
        float3 newOffset = lerp(pd.offset, targetOffset, 0.02);

        // Clamp to the authored cell — going further would put the probe
        // inside a neighbouring cell and break the trilinear blend at sample
        // time.
        const float3 limit = g_Vol.probeSpacing * 0.45;
        newOffset = clamp(newOffset, -limit, limit);

        pd.offset = newOffset;
    }

    g_ProbeData[probeIdx] = pd;
}
