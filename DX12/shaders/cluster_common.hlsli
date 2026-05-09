// cluster_common.hlsli — shared definitions for clustered deferred shading.
//
// Used by: ClusterBuild.cs.hlsl, ClusterCull.cs.hlsl, Lighting.ps.hlsl

#ifndef CLUSTER_COMMON_HLSLI
#define CLUSTER_COMMON_HLSLI

// ---- Grid constants --------------------------------------------------------
// These must match the C++ side (ClusterPass.h).
#define CLUSTER_TILE_X   16
#define CLUSTER_TILE_Y    9
#define CLUSTER_SLICES   24
#define CLUSTER_COUNT    (CLUSTER_TILE_X * CLUSTER_TILE_Y * CLUSTER_SLICES)

#define MAX_LIGHTS_PER_CLUSTER 256
#define MAX_LIGHT_INDEX_COUNT  (CLUSTER_COUNT * 32)  // average ~32 lights per cluster budget

// ---- Structures ------------------------------------------------------------

struct ClusterAABB
{
    float3 minPt;
    float  _pad0;
    float3 maxPt;
    float  _pad1;
};

struct GPULight
{
    float3 position;
    float  radius;
    float3 color;
    float  intensity;
    float3 direction;
    float  spotAngle;       // half-angle in radians
    uint   type;            // 0=directional, 1=point, 2=spot (must match C++ LightType)
    uint   shadowSliceIdx;  // 0xFFFFFFFF = no shadow map; else slice in SpotShadowAtlas
    uint   _pad0;
    uint   _pad1;
};

struct LightGridEntry
{
    uint offset;
    uint count;
};

// ---- Helpers ---------------------------------------------------------------

uint ClusterIndex(uint3 coord)
{
    return coord.x
         + coord.y * CLUSTER_TILE_X
         + coord.z * CLUSTER_TILE_X * CLUSTER_TILE_Y;
}

uint3 GetClusterCoord(float2 screenUV, float linearDepth, float nearZ, float farZ)
{
    uint tileX = (uint)(screenUV.x * CLUSTER_TILE_X);
    uint tileY = (uint)(screenUV.y * CLUSTER_TILE_Y);

    // Exponential Z-slice: denser near the camera.
    float logRatio = log(linearDepth / nearZ) / log(farZ / nearZ);
    uint  slice    = (uint)(logRatio * CLUSTER_SLICES);
    slice = clamp(slice, 0, CLUSTER_SLICES - 1);

    return uint3(
        min(tileX, CLUSTER_TILE_X - 1),
        min(tileY, CLUSTER_TILE_Y - 1),
        slice);
}

// Sphere vs AABB overlap test.
bool SphereVsAABB(float3 center, float radius, ClusterAABB aabb)
{
    float3 closest = clamp(center, aabb.minPt, aabb.maxPt);
    float3 diff = closest - center;
    return dot(diff, diff) <= (radius * radius);
}

#endif // CLUSTER_COMMON_HLSLI
