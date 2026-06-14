// Terrain.shadow.ms.hlsl — depth-only mesh shader for terrain shadow casting.
//
// Same multi-tile heightmap-displacement geometry as Terrain.ms.hlsl, but
//   - projects with the per-cascade light VP (b1 ShadowPerViewCB) instead
//     of the camera viewProj
//   - emits only SV_Position (no per-vertex attributes; PS is null)
// ShadowPass calls DispatchMesh on this once per cascade, after rendering
// regular DrawPackets, so terrain ends up in the same Texture2DArray
// depth slice the rest of the scene's casters wrote to.

#include "Terrain.hlsli"

#define GRID_DIM      12
#define VERT_COUNT    (GRID_DIM * GRID_DIM)
#define PRIM_COUNT    ((GRID_DIM - 1) * (GRID_DIM - 1) * 2)
#define THREAD_COUNT  64
#define AS_GROUP_SIZE 32                                  // must match Terrain.as.hlsl

struct TerrainPayload
{
    uint subTileIdx[AS_GROUP_SIZE];
};

// Per-cascade shadow VP — bound by ShadowPass at b1 space0. Layout matches
// Shadow.vs.hlsl's ShadowPerViewCB.
cbuffer ShadowPerViewCB : register(b1, space0)
{
    float4x4 g_shadowVP;
};

// TerrainCB — geometry prefix only. The same Renderer CB feeds the colour and
// shadow passes; the shadow MS reads ONLY the geometry fields (worldOrigin,
// worldSize, heightScale, UV remap, tile count), so it declares just the prefix
// (declaring fewer trailing fields than the bound buffer is legal in HLSL).
cbuffer TerrainCB : register(b2, space0)
{
    float2 g_worldOrigin;
    float  g_worldSize;
    float  g_heightScale;

    float2 g_heightmapUVOffset;
    float2 g_heightmapUVScale;

    float  g_heightmapTexel;
    uint   g_hasHeightmap;
    uint   _hasSplatmap;
    float  g_worldCenterY;

    uint   g_tilesPerSide;
    uint   _g_enableFrustumCull;
    uint   _g_layerCount;
    uint   _g_pad0;
};

Texture2D<float>  g_HeightMap   : register(t2, space0);
SamplerState      g_LinearClamp : register(s0, space0);

// Same height function as the main terrain MS so vertex positions match
// exactly between the colour pass and the shadow pass — no z-fighting at
// silhouette edges.
float SampleHeight(float2 uv)
{
    if (g_hasHeightmap == 0) return g_worldCenterY;
    float h = TerrainSampleHeightBicubic(g_HeightMap, g_LinearClamp, uv, g_heightmapTexel);
    return g_worldCenterY + h * g_heightScale;
}

struct VertexOut
{
    float4 sv : SV_Position;
};

[outputtopology("triangle")]
[numthreads(THREAD_COUNT, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    in   payload TerrainPayload payload,
    out  vertices VertexOut verts[VERT_COUNT],
    out  indices  uint3     tris [PRIM_COUNT])
{
    SetMeshOutputCounts(VERT_COUNT, PRIM_COUNT);

    const uint  subTileIdx  = payload.subTileIdx[gid];
    const uint  N           = max(g_tilesPerSide, 1u);
    const uint  subX        = subTileIdx % N;
    const uint  subZ        = subTileIdx / N;
    const float macroSize   = g_worldSize;
    const float subTileSize = macroSize / float(N);
    const float2 tileOrigin = g_worldOrigin + float2(subX, subZ) * subTileSize;
    const float  step       = subTileSize / float(GRID_DIM - 1);

    [unroll(4)]
    for (uint v = gtid; v < VERT_COUNT; v += THREAD_COUNT)
    {
        uint2  gridXY     = uint2(v % GRID_DIM, v / GRID_DIM);
        float2 worldXZ    = tileOrigin + float2(gridXY) * step;
        float2 globalNorm = (worldXZ - g_worldOrigin) / macroSize;
        float2 hmUV       = g_heightmapUVOffset + globalNorm * g_heightmapUVScale;

        float  h        = SampleHeight(hmUV);
        float3 worldPos = float3(worldXZ.x, h, worldXZ.y);

        VertexOut o;
        o.sv = mul(float4(worldPos, 1.0), g_shadowVP);
        verts[v] = o;
    }

    [unroll(4)]
    for (uint p = gtid; p < PRIM_COUNT; p += THREAD_COUNT)
    {
        uint quadIdx = p / 2;
        uint quadX   = quadIdx % (GRID_DIM - 1);
        uint quadY   = quadIdx / (GRID_DIM - 1);

        uint i0 = quadY * GRID_DIM + quadX;
        uint i1 = i0 + 1;
        uint i2 = i0 + GRID_DIM;
        uint i3 = i2 + 1;

        // Same winding as the colour pass so cull_mode=BACK culls the same
        // faces. Shadow rendering uses cull_mode=BACK by default; the depth
        // bias on the ShadowPass PSO handles acne.
        tris[p] = (p & 1) ? uint3(i1, i2, i3) : uint3(i0, i2, i1);
    }
}
