// DecalClusterCull.cs.hlsl — assigns decals to view-space clusters.
//
// Dispatch: (CLUSTER_COUNT + 63) / 64, 1, 1
// Each thread handles one cluster. Fixed-layout indexing:
//   cluster i writes its decal index list to
//   [i * MAX_DECALS_PER_CLUSTER .. (i+1) * MAX_DECALS_PER_CLUSTER).
// No atomic counter — each cluster owns its own slice.
//
// Input: g_ClusterAABBs (built by ClusterBuild.cs.hlsl, shared with light cull)
//        g_Decals       (uploaded each frame by DecalPass::SetDecals)
// Output: per-cluster DecalGridEntry {offset, count} and packed index list.

#include "decal_common.hlsli"

cbuffer DecalCB : register(b0, space2)
{
    float4x4 invProj;        // unused here, kept for CB layout parity with ClusterCB
    float    nearZ;
    float    farZ;
    uint     screenW;
    uint     screenH;
    uint     decalCount;
    uint3    _pad;
    float4x4 viewMatrix;     // world → view (for transforming bounds into cluster space)
};

StructuredBuffer<ClusterAABB>     g_ClusterAABBs  : register(t0, space2);
StructuredBuffer<GPUDecal>        g_Decals        : register(t1, space2);

RWStructuredBuffer<uint>          g_DecalIndexList : register(u0, space2);
RWStructuredBuffer<DecalGridEntry> g_DecalGrid     : register(u1, space2);

[numthreads(64, 1, 1)]
void CSCullDecals(uint3 id : SV_DispatchThreadID)
{
    uint clusterIdx = id.x;
    if (clusterIdx >= CLUSTER_COUNT) return;

    ClusterAABB aabb = g_ClusterAABBs[clusterIdx];

    // Accumulate into registers first so we can sort by sortLayer before
    // writing the flattened index list. N = MAX_DECALS_PER_CLUSTER (= 16)
    // is small enough that insertion sort is cheaper than any alternative.
    uint  localIdx [MAX_DECALS_PER_CLUSTER];
    float localSort[MAX_DECALS_PER_CLUSTER];
    uint  count = 0;

    for (uint i = 0; i < decalCount && count < MAX_DECALS_PER_CLUSTER; ++i)
    {
        GPUDecal d = g_Decals[i];

        // Transform bounds sphere center to view space. radius is scale-invariant
        // (CPU side already accounts for the decal OBB's worst-case radius).
        float3 centerVS = mul(float4(d.boundsCenter, 1.0), viewMatrix).xyz;

        if (!DecalSphereVsAABB(centerVS, d.boundsRadius, aabb))
            continue;

        // Insertion sort on-the-fly: find the spot where d.sortLayer fits
        // (ascending), shift down, insert. Ties preserve insertion order
        // (stable) so same-layer decals blend in decal-buffer order, which
        // matches the CPU-side upload order — predictable for artists.
        const float s = d.sortLayer;
        uint pos = count;
        [loop]
        while (pos > 0 && localSort[pos - 1] > s)
        {
            localIdx [pos] = localIdx [pos - 1];
            localSort[pos] = localSort[pos - 1];
            --pos;
        }
        localIdx [pos] = i;
        localSort[pos] = s;
        ++count;
    }

    const uint baseOffset = clusterIdx * MAX_DECALS_PER_CLUSTER;
    for (uint j = 0; j < count; ++j)
        g_DecalIndexList[baseOffset + j] = localIdx[j];

    g_DecalGrid[clusterIdx].offset = baseOffset;
    g_DecalGrid[clusterIdx].count  = count;
}
