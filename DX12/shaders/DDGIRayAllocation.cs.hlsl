// DDGIRayAllocation.cs.hlsl — pack per-probe ray descriptors into a flat list.
//
// Each thread = one probe. Reads the bucket count for its probe from
// g_RayCount, atomically claims a slice in g_RayAlloc[4..], and writes its
// (probeIdx | rayIdx<<20) descriptors there.
//
// After this CS:
//   g_RayAlloc[3]  = total rays this frame
//   g_RayAlloc[4..]= flat array of packed (probe,ray) descriptors
//
// Dispatch args (g_RayAlloc[0..2]) are computed by DDGIFinalizeIndirect.cs.

#include "DDGICommon.hlsli"

ConstantBuffer<DDGIVolumeGPU>  g_Vol      : register(b0, space0);
RWStructuredBuffer<uint>       g_RayCount : register(u2, space0);
RWStructuredBuffer<uint>       g_RayAlloc : register(u3, space0);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint probeCount = g_Vol.probeCountsX * g_Vol.probeCountsY * g_Vol.probeCountsZ;
    const uint probeIdx = DTid.x;
    if (probeIdx >= probeCount) return;

    const uint rayCount = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;
    if (rayCount == 0) return;

    uint base;
    InterlockedAdd(g_RayAlloc[0], rayCount, base);

    // Bound check against the allocated descriptor capacity. Capacity is
    // (totalProbes * raysPerProbe) — exceeding it would corrupt out-of-range
    // memory. Should never happen since per-probe count ≤ raysPerProbe and
    // the buffer is sized for the max.
    const uint capacity = probeCount * g_Vol.raysPerProbe;
    if (base + rayCount > capacity) return;

    // probeIdx fits in 20 bits (≤ 1M probes); rayIdx fits in the upper 12 bits
    // (≤ 4096 rays per probe). Both well within the practical engine limits.
    [loop] for (uint r = 0; r < rayCount; ++r)
    {
        g_RayAlloc[1 + base + r] = (probeIdx & 0xFFFFFu) | (r << 20u);
    }
}
