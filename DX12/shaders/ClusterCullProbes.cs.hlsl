// ClusterCullProbes.cs.hlsl — assigns reflection probes to clusters.
// Mirrors ClusterCull.cs.hlsl shape: one thread per cluster, fixed slice
// allocation in the index list (no atomic counter needed).
//
// Probe sphere = (probe.position, probe.influenceRadius) — set by the CPU
// upload to the bounding sphere of the OUTER falloff box.
//
// MAX_PROBES_PER_CLUSTER intentionally small (8) since per-pixel probe
// loops dominate cost in the lighting pass; widening it amplifies the
// shader-time hit. Bump if a scene reliably overflows.

#include "cluster_common.hlsli"

#define MAX_PROBES_PER_CLUSTER 8

struct ReflectionProbe
{
    float3 position;        float influenceRadius;
    float3 boxMin;          uint   cubemapSlice;
    float3 boxMax;          uint   flags;
    float3 innerExtents;    float  intensity; // unused here; keeps layout in sync
};

struct ProbeGridEntry
{
    uint offset;
    uint count;
};

cbuffer ClusterCB : register(b0, space2)
{
    float4x4 invProj;
    float    nearZ;
    float    farZ;
    uint     screenW;
    uint     screenH;
    uint     lightCount;
    uint     probeCount;     // NEW — count of valid entries in g_Probes
    uint2    _pad;
    float4x4 viewMatrix;
};

StructuredBuffer<ClusterAABB>      g_ClusterAABBs   : register(t0, space2);
StructuredBuffer<ReflectionProbe>  g_Probes         : register(t2, space2);

RWStructuredBuffer<uint>           g_ProbeIndexList : register(u0, space2);
RWStructuredBuffer<ProbeGridEntry> g_ProbeGrid      : register(u1, space2);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint clusterIdx = id.x;
    if (clusterIdx >= CLUSTER_COUNT) return;

    ClusterAABB aabb = g_ClusterAABBs[clusterIdx];

    uint baseOffset = clusterIdx * MAX_PROBES_PER_CLUSTER;
    uint count = 0;

    for (uint i = 0; i < probeCount && count < MAX_PROBES_PER_CLUSTER; i++)
    {
        ReflectionProbe p = g_Probes[i];
        if (p.influenceRadius <= 0.0) continue;  // unassigned slot guard

        float3 viewPos = mul(float4(p.position, 1.0), viewMatrix).xyz;
        if (SphereVsAABB(viewPos, p.influenceRadius, aabb))
        {
            g_ProbeIndexList[baseOffset + count] = i;
            count++;
        }
    }

    g_ProbeGrid[clusterIdx].offset = baseOffset;
    g_ProbeGrid[clusterIdx].count  = count;
}
