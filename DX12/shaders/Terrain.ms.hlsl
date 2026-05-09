// Terrain.ms.hlsl — Mesh-shader terrain (multi-tile dispatch).
//
// One thread group emits one sub-tile of a `tilesPerSide × tilesPerSide`
// macro grid. Per sub-tile: 12×12 vertices → 144 verts / 242 prims, well
// under the DX12 256/256 MS limits. Heightmap is sampled bicubically (see
// Terrain.hlsli) so vertex displacement is C²-continuous even when the
// source asset is BC-compressed or 8-bit.

#include "Terrain.hlsli"

#define GRID_DIM      12
#define VERT_COUNT    (GRID_DIM * GRID_DIM)                  // 144
#define PRIM_COUNT    ((GRID_DIM - 1) * (GRID_DIM - 1) * 2)  // 242
#define THREAD_COUNT  64
#define AS_GROUP_SIZE 32                                     // must match Terrain.as.hlsl

// Payload from Terrain.as.hlsl. The AS does the frustum cull then writes
// surviving sub-tile indices here in compacted order. gid in this MS is
// the index into that compacted list.
struct TerrainPayload
{
    uint subTileIdx[AS_GROUP_SIZE];
};

// ---- Bindings --------------------------------------------------------------

cbuffer PerViewCB : register(b1, space0)
{
    // Mirrors the Renderer's PerView CB layout (same as GBuffer.vs.hlsl) so we
    // can reuse the existing root-CBV slot. Only viewProj is needed for the
    // terrain MS — the velocity / prev-frame matrices are read by the PS.
    float4x4 g_viewProj;
    float4x4 g_prevViewProj;
    float4x4 g_curViewProjNoJitter;
};

// Layout MUST match Terrain.ps.hlsl and the TerrainParamsCB struct
// in Renderer.cpp — every field offset is shared with the PS view.
cbuffer TerrainCB : register(b2, space0)
{
    float2 g_worldOrigin;       // tile bottom-left in world XZ
    float  g_worldSize;
    float  g_heightScale;

    float2 g_heightmapUVOffset;
    float2 g_heightmapUVScale;

    float  g_heightmapTexel;    // 1.0 / heightmap resolution (in UV units of the FULL map)
    uint   g_hasHeightmap;      // 0 = render flat
    uint   g_hasSplatmap;       // PS-only — read for layout parity
    float  g_worldCenterY;      // tile pivot Y; displacement is centred here

    int4   g_layerBindlessIdx;  // PS-only
    float4 g_layerTilingScale;  // PS-only
    int4   g_layerNormalIdx;    // PS-only
    int4   g_layerARMIdx;       // PS-only
    int4   g_layerDispIdx;      // PS-only

    uint   g_tilesPerSide;
    uint   _g_enableFrustumCull;   // AS-only — laid out for parity
    float  _g_pad8a;
    float  _g_pad8b;

    float4 _g_layerMinHeight;      // PS-only — laid out for parity
    float4 _g_layerMaxHeight;      // PS-only
    float4 _g_layerFadeHeight;     // PS-only
    float4 _g_layerMinSlopeDeg;    // PS-only
    float4 _g_layerMaxSlopeDeg;    // PS-only
    float4 _g_layerFadeSlopeDeg;   // PS-only

    float4 _g_frustumPlanes[6];    // AS-only — laid out for parity
};

Texture2D<float>  g_HeightMap   : register(t2, space0);
SamplerState      g_LinearClamp : register(s0, space0);

// ---- Output struct ---------------------------------------------------------

struct VertexOut
{
    float4 sv        : SV_Position;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
    float4 curClip   : TEXCOORD1;
    float4 prevClip  : TEXCOORD2;
};

// ---- Heightmap sampling ----------------------------------------------------

// Returns world-space Y for a heightmap UV. g_worldCenterY is the BASE
// (floor) of the terrain: heightmap value 0 maps to worldCenterY, value 1
// maps to worldCenterY + heightScale. So bumping g_heightScale only
// raises the peaks; the base stays anchored to the pivot. Uses cubic
// B-spline filtering (4 bilinear taps) so the surface is C2-continuous
// even when the heightmap is quantised — no contour-map terracing.
float SampleHeight(float2 uv)
{
    if (g_hasHeightmap == 0) return g_worldCenterY;
    float h = TerrainSampleHeightBicubic(g_HeightMap, g_LinearClamp, uv, g_heightmapTexel);
    return g_worldCenterY + h * g_heightScale;
}

// ---- Main ------------------------------------------------------------------

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

    // Multi-tile: one mesh-shader thread group emits one sub-tile. The AS
    // produced a compacted payload of visible sub-tile indices; gid in
    // [0, visibleCount) maps back to the original sub-tile via the
    // payload, then to (subX, subZ) using g_tilesPerSide.
    const uint  subTileIdx  = payload.subTileIdx[gid];
    const uint  N           = max(g_tilesPerSide, 1u);
    const uint  subX        = subTileIdx % N;
    const uint  subZ        = subTileIdx / N;
    const float macroSize   = g_worldSize;
    const float subTileSize = macroSize / float(N);

    // Bottom-left corner of THIS sub-tile in world XZ.
    const float2 tileOrigin = g_worldOrigin + float2(subX, subZ) * subTileSize;
    // Step across one sub-tile quad (so neighbouring sub-tiles share
    // exactly the same world-XZ at their shared edge → no T-junctions).
    const float  step       = subTileSize / float(GRID_DIM - 1);

    // ---- Vertices: each thread writes a strided subset ---------------------
    [unroll(4)]
    for (uint v = gtid; v < VERT_COUNT; v += THREAD_COUNT)
    {
        uint2 gridXY = uint2(v % GRID_DIM, v / GRID_DIM);
        float2 worldXZ = tileOrigin + float2(gridXY) * step;

        // Heightmap UV from GLOBAL macro-tile position so every sub-tile
        // samples its slice of the same heightmap consistently.
        float2 globalNorm = (worldXZ - g_worldOrigin) / macroSize;
        float2 hmUV       = g_heightmapUVOffset + globalNorm * g_heightmapUVScale;

        // Displaced world position.
        float h = SampleHeight(hmUV);
        float3 worldPos = float3(worldXZ.x, h, worldXZ.y);

        // Analytic normal from height derivatives — finite difference on the
        // heightmap with a 1-texel offset in UV space. World-space scale of one
        // texel along X (or Z) is (heightmapTexel * uvScale.x) * worldSize.
        float texelU = g_heightmapTexel * g_heightmapUVScale.x;
        float texelV = g_heightmapTexel * g_heightmapUVScale.y;
        // World distance per heightmap texel uses the MACRO tile size — the
        // heightmap covers the whole macro tile regardless of sub-tile count.
        float worldTexelX = g_heightmapTexel * macroSize;
        float worldTexelZ = g_heightmapTexel * macroSize;

        float hL = SampleHeight(hmUV + float2(-texelU, 0));
        float hR = SampleHeight(hmUV + float2( texelU, 0));
        float hD = SampleHeight(hmUV + float2(0, -texelV));
        float hU = SampleHeight(hmUV + float2(0,  texelV));

        float3 normalWS = normalize(float3(hL - hR,
                                           2.0 * worldTexelX,
                                           hD - hU));
        float3 tangentWS = normalize(float3(2.0 * worldTexelX, hR - hL, 0));
        float3 bitangentWS = cross(normalWS, tangentWS);

        VertexOut o;
        o.sv       = mul(float4(worldPos, 1.0), g_viewProj);
        o.worldPos = worldPos;
        o.uv       = hmUV;
        o.wn       = normalWS;
        o.wt       = tangentWS;
        o.wbt      = bitangentWS;
        o.col      = float3(1, 1, 1);
        o.curClip  = mul(float4(worldPos, 1.0), g_curViewProjNoJitter);
        o.prevClip = mul(float4(worldPos, 1.0), g_prevViewProj);

        verts[v] = o;
    }

    // ---- Indices: each thread emits a strided subset of quads --------------
    // Engine-default rasterizer state is `front_counter_clockwise=false`
    // (CW=front) with cull_mode=BACK. Vertex layout in world XZ is
    //   i0=(X,  Z  )   i1=(X+1, Z  )
    //   i2=(X,  Z+1)   i3=(X+1, Z+1)
    // so the order (i0, i2, i1) gives a face whose geometric normal is +Y
    // (top), which is what we want visible from above.
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

        // Both triangles wound so the geometric normal is +Y (top facing up).
        tris[p] = (p & 1) ? uint3(i1, i2, i3) : uint3(i0, i2, i1);
    }
}
