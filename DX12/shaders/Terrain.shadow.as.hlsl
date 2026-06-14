// Terrain.shadow.as.hlsl — Amplification shader for the shadow pass.
//
// Per-cascade frustum cull. Each AS thread evaluates one sub-tile's
// world-space AABB against the 6 frustum planes of the CURRENT shadow
// cascade (published by ShadowPass in ShadowPerViewCB at b1). Surviving
// sub-tile indices are wave-compacted and forwarded to the depth-only
// MS via DispatchMesh.
//
// Why a separate AS from Terrain.as.hlsl: the colour-pass AS culls
// against the CAMERA frustum (in TerrainCB). Reusing it here would make
// the shadow path drop sub-tiles that the camera can't see but the
// LIGHT can — i.e. it would over-cull and break self-shadow at glancing
// angles. ShadowPass uploads per-cascade planes into b1 each frame, so
// each cascade's dispatch automatically picks up the right planes.

#define AS_GROUP_SIZE 32

// ShadowPerViewCB — written by ShadowPass per cascade. Layout:
//   float4x4 shadowViewProj;        (only used by Shadow.vs.hlsl + MS)
//   float4   shadowFrustumPlanes[6];  (used here for culling)
cbuffer ShadowPerViewCB : register(b1, space0)
{
    float4x4 _g_shadowVP;
    float4   g_shadowFrustumPlanes[6];
};

// TerrainCB — geometry prefix only (the shadow AS culls against the per-cascade
// planes in b1, NOT TerrainCB). Declaring fewer trailing fields than the bound
// buffer is legal; offsets of the declared geometry fields match Renderer.h.
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

    uint   g_tilesPerSide;
    uint   _g_enableFrustumCull;
    uint   _g_layerCount;
    uint   _g_pad0;
};

struct TerrainPayload
{
    uint subTileIdx[AS_GROUP_SIZE];
};

groupshared TerrainPayload s_payload;

// AABB-vs-frustum p-vertex test against the per-cascade planes.
bool SubTileVisible(uint subTileIdx)
{
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
        float4 plane = g_shadowFrustumPlanes[p];
        float3 corner;
        corner.x = (plane.x >= 0) ? aabbMax.x : aabbMin.x;
        corner.y = (plane.y >= 0) ? aabbMax.y : aabbMin.y;
        corner.z = (plane.z >= 0) ? aabbMax.z : aabbMin.z;
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

    uint laneCount = WaveActiveCountBits(visible);
    uint laneIdx   = WavePrefixCountBits(visible);

    if (visible)
        s_payload.subTileIdx[laneIdx] = subTileIdx;

    DispatchMesh(laneCount, 1, 1, s_payload);
}
