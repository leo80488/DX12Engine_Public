// InstanceCull.cs.hlsl — GPU frustum culling compute shader.
//
// One thread per input instance. Tests world-space AABB against 6 frustum planes.
// Surviving instances are appended to the output IndirectDrawCommand buffer.
//
// Root signature (culling-specific, space2):
//   [0] CBV  b0 space2  — CullingCB (frustum planes, instance count)
//   [1] SRV  t0 space2  — GPUInstanceData[] (input)
//   [2] SRV  t1 space2  — MeshAABB[] (per-mesh local AABB)
//   [3] UAV  u0 space2  — IndirectDrawCommand[] (output)
//   [4] UAV  u1 space2  — uint drawCount (atomic counter)

#include "gpu_instance.hlsli"

struct MeshAABB
{
    float3 aabbMin;
    float3 aabbMax;
};

cbuffer CullingCB : register(b0, space2)
{
    float4   frustumPlanes[6]; // xyz=normal, w=distance (inward-pointing)
    float4x4 viewProj;
    uint     instanceCount;
    uint3    _pad;
};

StructuredBuffer<GPUInstanceData> g_Instances : register(t0, space2);
StructuredBuffer<MeshAABB>        g_MeshAABBs : register(t1, space2);

// Output: IndirectDrawCommand layout = 4 root constants + 4 draw args = 8 uint32
struct IndirectDrawCommand
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIdx;
    uint prevPosInfo;
    uint vertexCount;
    uint instanceCount;
    uint startVertex;
    uint startInstance;
};

RWStructuredBuffer<IndirectDrawCommand> g_OutCommands : register(u0, space2);
RWStructuredBuffer<uint>               g_DrawCount   : register(u1, space2);

#if OCCLUSION_CULL
Texture2D<float> g_HiZ : register(t2, space2);
SamplerState     g_HiZSampler : register(s0, space2); // point-clamp
#endif

// ---- Frustum test: AABB vs 6 planes ----
bool FrustumTestWorldAABB(float3 aabbMin, float3 aabbMax)
{
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        float3 n = frustumPlanes[i].xyz;
        float  d = frustumPlanes[i].w;

        // Positive vertex: corner most along the plane normal
        float3 pv;
        pv.x = (n.x >= 0) ? aabbMax.x : aabbMin.x;
        pv.y = (n.y >= 0) ? aabbMax.y : aabbMin.y;
        pv.z = (n.z >= 0) ? aabbMax.z : aabbMin.z;

        if (dot(n, pv) + d < 0)
            return false; // fully outside this plane
    }
    return true;
}

// Transform local AABB by world matrix → compute world-space AABB
void TransformAABB(float4x4 world, float3 localMin, float3 localMax,
                   out float3 worldMin, out float3 worldMax)
{
    // Extract translation
    float3 t = float3(world[3][0], world[3][1], world[3][2]);

    worldMin = t;
    worldMax = t;

    // For each axis of the world matrix
    [unroll]
    for (int i = 0; i < 3; i++)
    {
        float3 axis = float3(world[i][0], world[i][1], world[i][2]);

        float3 a = axis * localMin[i];
        float3 b = axis * localMax[i];

        worldMin += min(a, b);
        worldMax += max(a, b);
    }
}

#if OCCLUSION_CULL
// Project world AABB to screen-space and test against Hi-Z mip chain.
// Returns true if the AABB is potentially visible (not fully occluded).
// Reversed-Z convention: NDC z=1 at near plane, z=0 at far plane. Closer
// geometry has LARGER z. The Hi-Z pyramid stores the MIN depth over each
// 2x2 block — under reversed Z that is the FARTHEST occluder in the tile,
// which is what conservative occlusion needs.
bool HiZOcclusionTest(float3 worldMin, float3 worldMax)
{
    // Project all 8 AABB corners to clip space and find screen-space bounds.
    float2 ssMin = float2( 1e10,  1e10);
    float2 ssMax = float2(-1e10, -1e10);
    // Track the AABB's NEAREST corner (largest NDC z under reversed Z).
    float  aabbNearZ = 0.0;

    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float3 corner = float3(
            (i & 1) ? worldMax.x : worldMin.x,
            (i & 2) ? worldMax.y : worldMin.y,
            (i & 4) ? worldMax.z : worldMin.z
        );

        float4 clip = mul(float4(corner, 1.0), viewProj);
        if (clip.w <= 0.0) return true; // behind camera → conservatively visible

        float3 ndc = clip.xyz / clip.w;
        float2 uv  = ndc.xy * float2(0.5, -0.5) + 0.5;

        ssMin = min(ssMin, uv);
        ssMax = max(ssMax, uv);
        aabbNearZ = max(aabbNearZ, ndc.z);
    }

    // Clamp to screen
    ssMin = saturate(ssMin);
    ssMax = saturate(ssMax);

    // Compute mip level: choose the mip that covers the projected size
    float2 extent = (ssMax - ssMin) * float2(1920, 1080); // approx screen resolution
    float  mipLevel = ceil(log2(max(extent.x, extent.y)));
    mipLevel = clamp(mipLevel, 0, 10);

    // Sample Hi-Z at the center of the projected AABB. hizDepth = min of the
    // tile under reversed Z = depth of the FARTHEST occluder in the tile.
    float2 center = (ssMin + ssMax) * 0.5;
    float  hizDepth = g_HiZ.SampleLevel(g_HiZSampler, center, mipLevel);

    // Visible when the AABB's nearest corner is at least as close as the
    // tile's farthest known occluder (aabbNearZ >= hizDepth under reversed Z).
    return aabbNearZ >= hizDepth;
}
#endif

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= instanceCount) return;

    GPUInstanceData inst = g_Instances[idx];
    MeshAABB meshAabb    = g_MeshAABBs[inst.meshDescIdx];

    // Transform local AABB to world space
    // Note: inst.world is transposed (row-major stored as column-major for mul())
    // We need to un-transpose for our manual AABB transform
    float4x4 w;
    w[0] = float4(inst.world[0][0], inst.world[1][0], inst.world[2][0], inst.world[3][0]);
    w[1] = float4(inst.world[0][1], inst.world[1][1], inst.world[2][1], inst.world[3][1]);
    w[2] = float4(inst.world[0][2], inst.world[1][2], inst.world[2][2], inst.world[3][2]);
    w[3] = float4(inst.world[0][3], inst.world[1][3], inst.world[2][3], inst.world[3][3]);

    float3 worldMin, worldMax;
    TransformAABB(w, meshAabb.aabbMin, meshAabb.aabbMax, worldMin, worldMax);

    // Frustum test
    if (!FrustumTestWorldAABB(worldMin, worldMax))
        return;

#if OCCLUSION_CULL
    // Hi-Z occlusion test (uses previous frame's depth mip chain)
    if (!HiZOcclusionTest(worldMin, worldMax))
        return;
#endif

    // Visible → append IndirectDrawCommand
    uint slot;
    InterlockedAdd(g_DrawCount[0], 1, slot);

    IndirectDrawCommand cmd;
    cmd.meshDescIdx    = inst.meshDescIdx;
    cmd.instanceOffset = idx;      // point to this instance in the instance buffer
    cmd.materialIdx    = inst.materialIdx;
    cmd.prevPosInfo    = 0xFFFFFFFF; // static (no skinned prev pos)
    cmd.vertexCount    = 0;        // TODO: need vertex count from MeshDescriptor
    cmd.instanceCount  = 1;
    cmd.startVertex    = 0;
    cmd.startInstance  = 0;

    g_OutCommands[slot] = cmd;
}
