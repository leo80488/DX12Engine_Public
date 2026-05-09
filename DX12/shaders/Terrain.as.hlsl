// Terrain.as.hlsl — Amplification shader: per-sub-tile frustum culling.
//
// One AS thread group = AS_GROUP_SIZE sub-tiles. Each thread evaluates a
// single sub-tile's world-space AABB against six frustum planes published
// by the renderer in TerrainCB (camera frustum for the colour pass; per-
// cascade light frustum for shadows). The group then emits one MS thread
// group per VISIBLE sub-tile via DispatchMesh, with a payload carrying
// the original sub-tile index so the MS can rebuild (subX, subZ).
//
// CPU-side dispatch count must be ceil(tilesPerSide² / AS_GROUP_SIZE).
// With tilesPerSide=128 and AS_GROUP_SIZE=32 that's 16384 / 32 = 512 AS
// groups, each emitting 0..32 MS groups. Worst-case (all visible) the
// total MS group count matches the old direct-dispatch path.
//
// Conservative AABB Y-range: [worldCenterY, worldCenterY + heightScale].
// A min/max heightmap pyramid would tighten this dramatically (most
// sub-tiles only span a fraction of the macro Y range), but that's a
// bigger investment — start with the conservative case which already
// reaps the wins for "looking sideways / up at the sky" cases.

#define AS_GROUP_SIZE 32

// CB layout MUST match Terrain.{ms,ps}.hlsl bit-for-bit.
cbuffer TerrainCB : register(b2, space0)
{
    float2 g_worldOrigin;
    float  g_worldSize;
    float  g_heightScale;

    float2 _g_heightmapUVOffset;
    float2 _g_heightmapUVScale;

    float  _g_heightmapTexel;
    uint   _g_hasHeightmap;
    uint   _g_hasSplatmap;
    float  g_worldCenterY;

    int4   _g_layerBindlessIdx;
    float4 _g_layerTilingScale;
    int4   _g_layerNormalIdx;
    int4   _g_layerARMIdx;
    int4   _g_layerDispIdx;

    uint   g_tilesPerSide;
    uint   g_enableFrustumCull;
    float  _g_pad8a;
    float  _g_pad8b;

    float4 _g_layerMinHeight;
    float4 _g_layerMaxHeight;
    float4 _g_layerFadeHeight;
    float4 _g_layerMinSlopeDeg;
    float4 _g_layerMaxSlopeDeg;
    float4 _g_layerFadeSlopeDeg;

    float4 g_frustumPlanes[6];     // inward-normal planes; dot(p.xyz, P) + p.w ≥ 0 ⇒ inside
};

struct TerrainPayload
{
    uint subTileIdx[AS_GROUP_SIZE];
};

groupshared TerrainPayload s_payload;

// AABB-vs-frustum p-vertex test. Returns true when the box is at least
// partially on the inside of every plane (standard conservative cull —
// no false negatives, may pass some boxes that touch the outside).
bool SubTileVisible(uint subTileIdx)
{
    if (g_enableFrustumCull == 0) return true;

    uint N = max(g_tilesPerSide, 1u);
    uint subX = subTileIdx % N;
    uint subZ = subTileIdx / N;

    float subTileSize = g_worldSize / float(N);
    float2 originXZ   = g_worldOrigin + float2(subX, subZ) * subTileSize;

    float3 aabbMin = float3(originXZ.x,                 g_worldCenterY,                  originXZ.y);
    float3 aabbMax = float3(originXZ.x + subTileSize,   g_worldCenterY + g_heightScale,  originXZ.y + subTileSize);

    [unroll]
    for (uint p = 0; p < 6; ++p)
    {
        float4 plane = g_frustumPlanes[p];
        // P-vertex: pick the corner most likely to be INSIDE the plane.
        // (For inward-normal planes that's the corner along +normal.)
        float3 corner;
        corner.x = (plane.x >= 0) ? aabbMax.x : aabbMin.x;
        corner.y = (plane.y >= 0) ? aabbMax.y : aabbMin.y;
        corner.z = (plane.z >= 0) ? aabbMax.z : aabbMin.z;
        // dot(plane.xyz, corner) + plane.w < 0 ⇒ entire box is outside this plane.
        if (dot(plane.xyz, corner) + plane.w < 0.0) return false;
    }
    return true;
}

[numthreads(AS_GROUP_SIZE, 1, 1)]
void main(uint gtid : SV_GroupThreadID,
          uint gid  : SV_GroupID)
{
    uint N             = max(g_tilesPerSide, 1u);
    uint totalSubTiles = N * N;
    uint subTileIdx    = gid * AS_GROUP_SIZE + gtid;

    bool visible = (subTileIdx < totalSubTiles) && SubTileVisible(subTileIdx);

    // Wave-level stream compaction. The AS group size is 32, which equals
    // a single wave on Turing+ / RDNA (most modern GPUs). On 64-lane
    // waves (older AMD GCN) the AS group still has 32 lanes, so 32 lanes
    // are inactive — wave intrinsics correctly only count active lanes,
    // so this stays correct, just leaves half the wave idle.
    uint laneCount = WaveActiveCountBits(visible);
    uint laneIdx   = WavePrefixCountBits(visible);

    if (visible)
        s_payload.subTileIdx[laneIdx] = subTileIdx;

    DispatchMesh(laneCount, 1, 1, s_payload);
}
