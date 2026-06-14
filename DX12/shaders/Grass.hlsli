#ifndef GRASS_HLSLI
#define GRASS_HLSLI

// Grass.hlsli — shared declarations for the GoT-style procedural grass pass.
//
// Ghost-of-Tsushima-inspired pipeline (GDC 2021 "Procedural Grass in
// 'Ghost of Tsushima'", adapted to this engine's AS→MS architecture):
//   - The grass field is a patchesPerSide × patchesPerSide grid of patches.
//   - Grass.as.hlsl: one AS thread per patch — frustum cull + distance cull,
//     pick a LOD (segment count / blades-per-group), compute how many MS
//     groups the patch needs, wave-prefix-sum them into a compacted payload.
//   - Grass.ms.hlsl: one MS group emits up to N blades. Every blade is
//     procedurally generated from a hash — no vertex/instance buffers exist
//     anywhere. A blade is a quadratic Bezier ribbon: root pinned to the
//     terrain heightmap, tip tilted/bent and displaced by wind noise.
//   - Grass.ps.hlsl: writes the deferred GBuffer (same 6 MRTs as Terrain.ps)
//     so grass receives CSM shadows / DDGI / SSAO through LightingPass with
//     zero special-casing downstream.
//
// CB layout MUST match GrassCBData in GrassPass.h bit-for-bit (20 rows).

cbuffer GrassCB : register(b2, space0)
{
    // row 0 — grass field placement (bottom-left corner, world XZ)
    float2 g_grassOrigin;
    float  g_grassSize;
    float  g_baseY;             // terrain base Y (heightmap value 0)

    // row 1
    float  g_heightScale;       // terrain heightmap world amplitude
    float  g_hmTexel;           // 1 / heightmap resolution
    uint   g_hasHeightmap;      // 0 → flat field at g_baseY
    uint   g_patchesPerSide;

    // row 2 — terrain heightmap UV remap (matches Terrain.ms.hlsl math)
    float2 g_hmUVOffset;
    float2 g_hmUVScale;

    // row 3 — the TERRAIN tile mapping (may differ from the grass field!)
    float2 g_terrainOrigin;
    float  g_terrainSize;
    float  _g_padT;

    // row 4
    float3 g_cameraPos;
    float  g_time;

    // row 5
    float  g_prevTime;
    float  g_lod0Dist;
    float  g_lod1Dist;
    float  g_cullDist;

    // row 6
    float  g_density;           // blades per m² at LOD0
    float  g_bladeHeight;
    float  g_bladeHeightVar;    // ±fraction of bladeHeight
    float  g_bladeWidth;        // half-profile base width (m)

    // row 7
    float  g_tiltMax;           // radians — max static lean from vertical
    float  g_bendAmount;        // Bezier mid-point slack [0..1]
    float  g_windStrength;      // tip displacement amplitude (m)
    float  g_windSpeed;

    // row 8
    float  g_windScale;         // noise cycles per metre
    float2 g_windDir;           // normalized XZ
    float  g_clumpCellSize;     // Voronoi-ish clump cell size (m)

    // row 9
    float  g_clumpBlend;        // 0 = independent blades, 1 = full clump identity
    float  g_minWorldY;         // no grass below (e.g. water level)
    float  g_maxWorldY;         // no grass above (e.g. snow line)
    float  g_maxSlopeCos;       // cos(max slope) — geometric normal·up gate

    // rows 10/11
    float4 g_baseColor;         // rgb, a unused
    float4 g_tipColor;

    // row 12
    float  g_colorNoiseScale;   // cycles per metre
    float  g_colorNoiseAmount;
    float  g_rootAO;            // AO at the blade root (1 at tip)
    float  g_normalBlend;       // blade normal → up-vector soften

    // row 13
    float  g_viewThicken;       // edge-on view width compensation [0..1]
    float  g_farWidthMul;       // LOD2 width multiplier (coverage keeper)
    float  g_roughness;
    uint   g_seed;

    // rows 14..19
    float4 g_grassFrustumPlanes[6];   // inward normals: dot(n,P)+d ≥ 0 inside
};

// ---- LOD table --------------------------------------------------------------
// segments / blades-per-MS-group per LOD. MS limits: 256 verts / 256 prims.
//   LOD0: 7 segs → 15 v / 13 t per blade × 16 blades = 240 v / 208 t
//   LOD1: 3 segs →  7 v /  5 t per blade × 32 blades = 224 v / 160 t
//   LOD2: 2 segs →  5 v /  3 t per blade × 48 blades = 240 v / 144 t
static const uint kGrassSegs  [3] = { 7, 3, 2 };
static const uint kGrassBPG   [3] = { 16, 32, 48 };  // blades per MS group
static const float kGrassDensityMul[3] = { 1.0, 0.45, 0.18 };
#define GRASS_MAX_BLADES_PER_PATCH 256
#define GRASS_AS_GROUP_SIZE 32

struct GrassPayload
{
    // Per-AS-lane entries (NOT compacted): a culled lane has bladeCount 0,
    // making its MS group range empty so the MS scan can never match it.
    uint patchIdx  [GRASS_AS_GROUP_SIZE];
    uint groupBase [GRASS_AS_GROUP_SIZE];   // exclusive prefix sum of group counts
    uint bladeCount[GRASS_AS_GROUP_SIZE];
    uint lodLevel  [GRASS_AS_GROUP_SIZE];
};

// ---- Hashing ----------------------------------------------------------------

uint GrassHashUint(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// [0,1) float from two uints (patch id, blade id, stream)
float GrassHash(uint a, uint b, uint stream)
{
    uint h = GrassHashUint(a * 0x9E3779B9u + b + stream * 0x85EBCA6Bu + g_seed);
    return float(h & 0x00FFFFFFu) / 16777216.0;
}

float2 GrassHash2(float2 p)
{
    p = float2(dot(p, float2(127.1, 311.7)),
               dot(p, float2(269.5, 183.3)));
    return frac(sin(p) * 43758.5453123);
}

// 8-bit bit reversal — maps blade index → scattered cell of a 16×16 grid so
// "take the first N blades" (distance density falloff) stays spatially
// uniform AND blade positions are stable across LOD/density changes (GoT:
// blades must not swim when density drops with distance).
uint GrassBitRev8(uint v)
{
    v = ((v & 0x55u) << 1) | ((v & 0xAAu) >> 1);
    v = ((v & 0x33u) << 2) | ((v & 0xCCu) >> 2);
    v = ((v & 0x0Fu) << 4) | ((v & 0xF0u) >> 4);
    return v;
}

// ---- Value noise (analytic, no texture) --------------------------------------

float GrassValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = GrassHash2(i + float2(0, 0)).x;
    float b = GrassHash2(i + float2(1, 0)).x;
    float c = GrassHash2(i + float2(0, 1)).x;
    float d = GrassHash2(i + float2(1, 1)).x;
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Wind sway in [0..1]: travelling sine front + gust noise field, both
// scrolling along windDir. Matches the GoT "scrolling noise" feel without a
// texture fetch (AS/MS friendly).
float GrassWindAmp(float2 xz, float t)
{
    float2 wdir = g_windDir;
    float wave = sin(dot(xz, wdir) * (g_windScale * 6.2831853) * 0.45
                     + t * g_windSpeed * 2.1) * 0.5 + 0.5;
    // Gust = TWO value-noise octaves scrolling at different rates. A single
    // octave stalls: the Hermite fade's time derivative is zero on every
    // lattice crossing, so the slow sway holds-then-lurches (~1 Hz at
    // defaults) — reads as dropped frames. Decorrelated octave rates keep
    // the derivative zeros from ever aligning.
    float2 p = xz * g_windScale - wdir * (t * g_windSpeed * 0.7);
    float gust = 0.62 * GrassValueNoise(p)
               + 0.38 * GrassValueNoise(p * 2.13 + 19.7 - wdir * (t * g_windSpeed * 0.37));
    // Coefficients sum to exactly 1.0 — the old 0.45/0.45 form exceeded the
    // saturate() ceiling during strong gusts and flat-topped (blades pinned
    // rigid at max displacement, the other half of the stiffness).
    return saturate(0.30 + 0.35 * gust + 0.35 * wave * gust);
}

// ---- Terrain height (must mirror Terrain.ms.hlsl SampleHeight) ---------------
// Callers supply the heightmap SRV + clamp sampler (registers differ per stage).

float GrassTerrainUVValid(float2 worldXZ, out float2 hmUV)
{
    float2 norm = (worldXZ - g_terrainOrigin) / max(g_terrainSize, 1e-3);
    hmUV = g_hmUVOffset + norm * g_hmUVScale;
    return (norm.x >= 0.0 && norm.x <= 1.0 && norm.y >= 0.0 && norm.y <= 1.0) ? 1.0 : 0.0;
}

#endif // GRASS_HLSLI
