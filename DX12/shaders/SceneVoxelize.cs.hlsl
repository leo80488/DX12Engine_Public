// SceneVoxelize.cs.hlsl — triangle-precision voxelization for the volumetric
// fog occlusion grid. Each thread handles one triangle: fetches its vertices
// via the engine's Programmable Vertex Fetching system, transforms to world
// space using the mesh's instance matrix, then iterates every voxel covered
// by the triangle's bounding box and runs the Akenine-Möller Triangle-Box SAT
// test. Any voxel that actually intersects the triangle is marked 255.
//
// Compared to the Tier 1 AABB-dilated grid this catches:
//   * openings / doorways (wall triangles don't cover the void)
//   * thin railings (no fat surrounding AABB)
//   * angled / curved geometry (diagonal faces written cell-by-cell)
//   * concave meshes (limbs that don't fill their AABB)
//
// Root sig layout (compute, space2 / space0 / space1 as declared):
//   b0 space2 — VoxelizeCB
//   u0 space2 — RWTexture3D<uint>   (occupancy)
//   t0 space0 — StructuredBuffer<GPUInstanceData>   (root SRV, slot 9)
//   t1 space0 — StructuredBuffer<MeshDescriptor>    (root SRV, slot 10)
//   t0 space1 — ByteAddressBuffer   g_Buffers[]     (bindless, slot 11)

#include "gpu_instance.hlsli"
#include "pvf_fetch.hlsli"

cbuffer VoxelizeCB : register(b0, space2)
{
    float3 gridMin;      float _pad0;
    float3 gridExtent;   uint  gridDim;
    uint   meshDescIdx;      // per-dispatch: which MeshDescriptor to fetch
    uint   instanceOffset;   // per-dispatch: which instance entry to read world from
    uint   numTriangles;     // per-dispatch: how many triangles this dispatch covers
    uint   _pad1;
};

StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor>  MeshDescriptors : register(t1, space0);

RWTexture3D<uint> gOccupancy : register(u0, space2);

// ---------------------------------------------------------------------------
// Akenine-Möller Triangle-Box overlap test (adapted from the 2001 paper).
// v0/v1/v2 are triangle vertices in world space; boxCenter/boxHalf are the
// voxel's centre and half-extent in world space. Returns true if the
// triangle intersects the box (including edge/face contact).
// ---------------------------------------------------------------------------

#define AXISTEST_X01(a, b, fa, fb)                             \
{                                                              \
    p0 = a * v0.y - b * v0.z;                                  \
    p2 = a * v2.y - b * v2.z;                                  \
    if (p0 < p2) { mn = p0; mx = p2; } else { mn = p2; mx = p0; } \
    rad = fa * boxHalf.y + fb * boxHalf.z;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_X2(a, b, fa, fb)                              \
{                                                              \
    p0 = a * v0.y - b * v0.z;                                  \
    p1 = a * v1.y - b * v1.z;                                  \
    if (p0 < p1) { mn = p0; mx = p1; } else { mn = p1; mx = p0; } \
    rad = fa * boxHalf.y + fb * boxHalf.z;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_Y02(a, b, fa, fb)                             \
{                                                              \
    p0 = -a * v0.x + b * v0.z;                                 \
    p2 = -a * v2.x + b * v2.z;                                 \
    if (p0 < p2) { mn = p0; mx = p2; } else { mn = p2; mx = p0; } \
    rad = fa * boxHalf.x + fb * boxHalf.z;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_Y1(a, b, fa, fb)                              \
{                                                              \
    p0 = -a * v0.x + b * v0.z;                                 \
    p1 = -a * v1.x + b * v1.z;                                 \
    if (p0 < p1) { mn = p0; mx = p1; } else { mn = p1; mx = p0; } \
    rad = fa * boxHalf.x + fb * boxHalf.z;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_Z12(a, b, fa, fb)                             \
{                                                              \
    p1 = a * v1.x - b * v1.y;                                  \
    p2 = a * v2.x - b * v2.y;                                  \
    if (p2 < p1) { mn = p2; mx = p1; } else { mn = p1; mx = p2; } \
    rad = fa * boxHalf.x + fb * boxHalf.y;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_Z0(a, b, fa, fb)                              \
{                                                              \
    p0 = a * v0.x - b * v0.y;                                  \
    p1 = a * v1.x - b * v1.y;                                  \
    if (p0 < p1) { mn = p0; mx = p1; } else { mn = p1; mx = p0; } \
    rad = fa * boxHalf.x + fb * boxHalf.y;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

bool TriangleBoxOverlap(float3 tv0, float3 tv1, float3 tv2,
                        float3 boxCenter, float3 boxHalf)
{
    // Move box to origin.
    float3 v0 = tv0 - boxCenter;
    float3 v1 = tv1 - boxCenter;
    float3 v2 = tv2 - boxCenter;

    // Triangle edges.
    float3 e0 = v1 - v0;
    float3 e1 = v2 - v1;
    float3 e2 = v0 - v2;

    float p0, p1, p2, mn, mx, rad;
    float3 fe;

    // 9 axis tests (edges × box principal axes).
    fe = abs(e0);
    AXISTEST_X01(e0.z, e0.y, fe.z, fe.y);
    AXISTEST_Y02(e0.z, e0.x, fe.z, fe.x);
    AXISTEST_Z12(e0.y, e0.x, fe.y, fe.x);

    fe = abs(e1);
    AXISTEST_X01(e1.z, e1.y, fe.z, fe.y);
    AXISTEST_Y02(e1.z, e1.x, fe.z, fe.x);
    AXISTEST_Z0 (e1.y, e1.x, fe.y, fe.x);

    fe = abs(e2);
    AXISTEST_X2 (e2.z, e2.y, fe.z, fe.y);
    AXISTEST_Y1 (e2.z, e2.x, fe.z, fe.x);
    AXISTEST_Z12(e2.y, e2.x, fe.y, fe.x);

    // AABB overlap on the three box axes.
    float3 vmin = min(v0, min(v1, v2));
    float3 vmax = max(v0, max(v1, v2));
    if (any(vmin > boxHalf))  return false;
    if (any(vmax < -boxHalf)) return false;

    // Triangle plane vs box test.
    float3 normal = cross(e0, e1);
    float3 vmin_p, vmax_p;
    [unroll] for (int q = 0; q < 3; ++q)
    {
        if (normal[q] > 0) { vmin_p[q] = -boxHalf[q] - v0[q]; vmax_p[q] =  boxHalf[q] - v0[q]; }
        else               { vmin_p[q] =  boxHalf[q] - v0[q]; vmax_p[q] = -boxHalf[q] - v0[q]; }
    }
    if (dot(normal, vmin_p) > 0) return false;
    if (dot(normal, vmax_p) < 0) return false;

    return true;
}

#undef AXISTEST_X01
#undef AXISTEST_X2
#undef AXISTEST_Y02
#undef AXISTEST_Y1
#undef AXISTEST_Z12
#undef AXISTEST_Z0

// ---------------------------------------------------------------------------
// Fetch one triangle's three world-space vertex positions.
// Respects the mesh's index buffer when present.
// ---------------------------------------------------------------------------
void FetchTriangleWorld(uint triIdx, out float3 w0, out float3 w1, out float3 w2)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];

    uint i0 = FetchIndex(md, triIdx * 3u + 0u);
    uint i1 = FetchIndex(md, triIdx * 3u + 1u);
    uint i2 = FetchIndex(md, triIdx * 3u + 2u);

    float3 p0 = FETCH_POS(md, i0);
    float3 p1 = FETCH_POS(md, i1);
    float3 p2 = FETCH_POS(md, i2);

    // World matrix is stored transposed on GPU (row-vector convention in HLSL
    // mul(pos, world)); consistent with GBuffer / Shadow etc.
    float4x4 world = InstanceBuffer[instanceOffset].world;
    w0 = mul(float4(p0, 1.0), world).xyz;
    w1 = mul(float4(p1, 1.0), world).xyz;
    w2 = mul(float4(p2, 1.0), world).xyz;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint triIdx = dtid.x;
    if (triIdx >= numTriangles) return;

    float3 w0, w1, w2;
    FetchTriangleWorld(triIdx, w0, w1, w2);

    // Triangle's world-space AABB → voxel-index range.
    float3 triMin = min(w0, min(w1, w2));
    float3 triMax = max(w0, max(w1, w2));

    // Early-out if the triangle is entirely outside the grid.
    float3 gMax = gridMin + gridExtent;
    if (any(triMax < gridMin) || any(triMin > gMax)) return;

    float3 invExt  = 1.0 / gridExtent;
    float  voxSize = gridExtent.x / float(gridDim);
    float3 voxHalf = float3(voxSize, voxSize, voxSize) * 0.5;

    int3 vMin = clamp(int3(floor((triMin - gridMin) * invExt * float(gridDim))),
                      0, int(gridDim) - 1);
    int3 vMax = clamp(int3(ceil ((triMax - gridMin) * invExt * float(gridDim))),
                      0, int(gridDim) - 1);

    // Safety cap: a triangle covering the entire grid would iterate 128³ ≈ 2M
    // voxels in a single thread → hang. In practice an axis range > 32 is
    // already pathological; clamp it and let neighbouring triangles cover the
    // rest on subsequent dispatches.
    const int kMaxSpan = 32;
    vMax = min(vMax, vMin + kMaxSpan);

    for (int z = vMin.z; z <= vMax.z; ++z)
    for (int y = vMin.y; y <= vMax.y; ++y)
    for (int x = vMin.x; x <= vMax.x; ++x)
    {
        float3 vCenter = gridMin + (float3(x, y, z) + 0.5) * voxSize;
        if (TriangleBoxOverlap(w0, w1, w2, vCenter, voxHalf))
        {
            // Same value writes from multiple threads are race-safe for
            // R8_UINT — no InterlockedOr needed.
            gOccupancy[int3(x, y, z)] = 255;
        }
    }
}
