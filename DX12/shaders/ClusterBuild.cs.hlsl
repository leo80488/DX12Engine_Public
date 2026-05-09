// ClusterBuild.cs.hlsl — compute shader that builds per-cluster view-space AABBs.
//
// Dispatch: (CLUSTER_TILE_X, CLUSTER_TILE_Y, CLUSTER_SLICES)
// Each thread computes the min/max view-space corners of one cluster tile+slice.

#include "cluster_common.hlsli"

cbuffer ClusterCB : register(b0, space2)
{
    float4x4 invProj;     // inverse projection matrix (row-vector convention)
    float    nearZ;
    float    farZ;
    uint     screenW;
    uint     screenH;
    uint     lightCount;
    uint3    _pad;
};

RWStructuredBuffer<ClusterAABB> g_ClusterAABBs : register(u0, space2);

// Convert screen-space UV + NDC depth to view-space position.
float3 ScreenToView(float2 uv, float ndcDepth)
{
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndcDepth, 1.0);
    float4 viewPos = mul(ndc, invProj);
    return viewPos.xyz / viewPos.w;
}

// Compute the linear depth for a given Z-slice index (exponential distribution).
float SliceDepth(uint slice)
{
    return nearZ * pow(farZ / nearZ, float(slice) / float(CLUSTER_SLICES));
}

[numthreads(1, 1, 1)]
void CSBuildClusters(uint3 id : SV_DispatchThreadID)
{
    uint tileX = id.x;
    uint tileY = id.y;
    uint slice = id.z;

    if (tileX >= CLUSTER_TILE_X || tileY >= CLUSTER_TILE_Y || slice >= CLUSTER_SLICES)
        return;

    // Screen-space UV bounds for this tile.
    float2 uvMin = float2(float(tileX) / CLUSTER_TILE_X,
                          float(tileY) / CLUSTER_TILE_Y);
    float2 uvMax = float2(float(tileX + 1) / CLUSTER_TILE_X,
                          float(tileY + 1) / CLUSTER_TILE_Y);

    // Depth range for this slice (view-space Z, positive = in front of camera).
    float zNear = SliceDepth(slice);
    float zFar  = SliceDepth(slice + 1);

    // Convert view-space Z to NDC depth. The engine uses REVERSED-Z
    // (projection built with XMMatrixPerspectiveFovLH(fov, aspect, farZ, nearZ)
    // i.e. near/far swapped). Under that convention:
    //   ndc.z = nearZ * (z - farZ) / (z * (nearZ - farZ))
    //   z = nearZ → ndc.z = 1  (close)
    //   z = farZ  → ndc.z = 0  (far)
    // Using the old forward-Z formula here produced NDC values that invProj
    // (reversed-Z) unprojected to wildly wrong view-space Z, so the cluster
    // AABBs didn't match the frustum — which made spot lights near the ground
    // pop in/out as the camera moved between adjacent (mis-sized) clusters.
    float ndcNear = nearZ * (zNear - farZ) / (zNear * (nearZ - farZ));
    float ndcFar  = nearZ * (zFar  - farZ) / (zFar  * (nearZ - farZ));

    // Build 8 corners of the frustum tile in view space, find AABB.
    float3 corners[8];
    corners[0] = ScreenToView(float2(uvMin.x, uvMin.y), ndcNear);
    corners[1] = ScreenToView(float2(uvMax.x, uvMin.y), ndcNear);
    corners[2] = ScreenToView(float2(uvMin.x, uvMax.y), ndcNear);
    corners[3] = ScreenToView(float2(uvMax.x, uvMax.y), ndcNear);
    corners[4] = ScreenToView(float2(uvMin.x, uvMin.y), ndcFar);
    corners[5] = ScreenToView(float2(uvMax.x, uvMin.y), ndcFar);
    corners[6] = ScreenToView(float2(uvMin.x, uvMax.y), ndcFar);
    corners[7] = ScreenToView(float2(uvMax.x, uvMax.y), ndcFar);

    float3 aabbMin = corners[0];
    float3 aabbMax = corners[0];
    [unroll]
    for (uint i = 1; i < 8; ++i)
    {
        aabbMin = min(aabbMin, corners[i]);
        aabbMax = max(aabbMax, corners[i]);
    }

    uint idx = ClusterIndex(uint3(tileX, tileY, slice));
    g_ClusterAABBs[idx].minPt = aabbMin;
    g_ClusterAABBs[idx].maxPt = aabbMax;
}
