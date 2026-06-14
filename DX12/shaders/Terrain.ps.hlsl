// Terrain.ps.hlsl — Phase 3: full PBR per layer (albedo / normal / ARM)
// with displacement-driven height-blending between layers. Per-pixel
// geometric normal is reconstructed from a bicubic-filtered heightmap
// (see Terrain.hlsli) so shading is smooth across triangle interiors and
// quantised heightmap data does not show as contour-line terraces.

// ---- DEBUG: visualise splat weights directly --------------------------------
// Flip to 1 to skip albedo/normal/arm sampling and emit the 4-layer splat
// as RGBA-mixed primary colours: BLUE=pebbles, YELLOW=stone, RED=grass,
// GREEN=rocks. Useful to confirm h is reaching the full 0..1 range across
// the terrain.
#define TERRAIN_DEBUG_SPLAT 0

#include "Terrain.hlsli"
//
// Per layer the PS samples:
//   albedo  → blended into the albedo GBuffer RT
//   normal  → DX-style tangent-space, transformed via the macro-tile TBN
//             into world space and blended
//   ARM     → AO (R), Roughness (G), Metalness (B), repacked into the
//             surface GBuffer RT (R=Roughness, G=Metalness, B=AO,
//             A=Reflectance)
//   disp    → single-channel height used to bias splat weights so layer
//             transitions follow the underlying micro-displacement
//             instead of being a flat lerp ("height-correlated splat").

// Geometry-only CB (shared with Terrain.ms/as.hlsl). Per-layer material data
// lives in the StructuredBuffer below, NOT here — adding a layer is a data edit.
cbuffer TerrainCB : register(b2, space0)
{
    float2 g_worldOrigin;
    float  g_worldSize;
    float  g_heightScale;

    float2 g_heightmapUVOffset;
    float2 g_heightmapUVScale;

    float  g_heightmapTexel;
    uint   g_hasHeightmap;
    uint   g_hasSplatmap;
    float  g_worldCenterY;

    uint   g_tilesPerSide;
    uint   _g_enableFrustumCull;   // AS-only — read here for layout parity
    uint   g_layerCount;           // # valid entries in g_TerrainLayers
    uint   g_heightBlendEnable;    // 0 = plain linear blend
    float  g_heightBlendStrength;  // disp bias on the base weight
    float  g_heightBlendRange;     // soft cutoff width around the local max
    float  _g_pad1;
    float  _g_pad2;
};

// One element per terrain layer. The PS loops [0, g_layerCount). Layout MUST
// match RendererDetail::TerrainLayerGPU (48 bytes). When a splatmap is set the
// first 4 layers map to its RGBA channels; otherwise each layer is gated by its
// world-meter height range AND degree slope range (both smoothstep-faded).
struct TerrainLayerGPU
{
    int   albedoIdx;
    int   normalIdx;
    int   armIdx;
    float tilingScale;
    float minHeight;
    float maxHeight;
    float fadeHeight;
    float minSlopeDeg;
    float maxSlopeDeg;
    float fadeSlopeDeg;
    int   dispIdx;        // displacement map for height-correlated blend (-1 = none)
    float _pad1;
};
#define MAX_TERRAIN_LAYERS 8

// Heightmap is bound at t2 space0 (descriptor-table SRV with ALL stage
// visibility), so the PS can re-sample it for per-pixel analytic normal.
Texture2D<float>  g_HeightMap    : register(t2, space0);
Texture2D<float4> g_Splatmap     : register(t3, space0);
StructuredBuffer<TerrainLayerGPU> g_TerrainLayers : register(t4, space0);
Texture2D         g_AllTextures[]: register(t0, space2);
SamplerState      g_LinearClamp  : register(s0, space0);
SamplerState      g_LinearWrap   : register(s1, space0);

struct PSIn
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

struct GOut
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 surface  : SV_TARGET2;
    float2 velocity : SV_TARGET3;
    float4 extra    : SV_TARGET4;   // shading-model scratch
    float4 sceneCol : SV_TARGET5;   // HdrSceneColor: emissive seed (additive lighting)
};

// ---- Per-pixel analytic geometric normal -----------------------------------
// Re-derives the heightmap surface frame at the pixel's world XZ rather than
// relying on the MS's per-vertex normal. Per-vertex normals only update at
// vertex granularity and the rasterizer linearly interpolates them across
// each triangle — for a heightmap with steep slopes that interpolation
// produces visible "faceted" creases at triangle edges. Sampling the
// heightmap directly in the PS gives a full-resolution smooth normal.
float HeightAt(float2 hmUV)
{
    if (g_hasHeightmap == 0) return g_worldCenterY;
    // Bicubic — must match the MS exactly so the per-pixel normal is
    // consistent with the geometry the rasterizer is shading.
    // Convention: h ∈ [0,1] → Y ∈ [worldCenterY, worldCenterY + heightScale].
    float h = TerrainSampleHeightBicubic(g_HeightMap, g_LinearClamp, hmUV, g_heightmapTexel);
    return g_worldCenterY + h * g_heightScale;
}

void ComputeTerrainTBN(float3 worldPos, out float3 N, out float3 T, out float3 B)
{
    if (g_hasHeightmap == 0)
    {
        N = float3(0, 1, 0);
        T = float3(1, 0, 0);
        B = float3(0, 0, 1);
        return;
    }

    float2 globalNorm = (worldPos.xz - g_worldOrigin) / max(g_worldSize, 1e-3);
    float2 hmUV       = g_heightmapUVOffset + globalNorm * g_heightmapUVScale;

    // Wider stencil — 4 texels — averages across BC7 4×4 block boundaries so
    // a pixel sitting astride a block boundary doesn't get a wildly different
    // gradient than its neighbour pixel. Visual artefact without this: the
    // derivative jumps every 4 texels (~2 m at 1000 m / 2048 texels), which
    // shows up as a darker "band" wherever the slope happens to cross a
    // block boundary diagonally.
    const float kStencilTexels = 4.0;
    float texelU    = kStencilTexels * g_heightmapTexel * g_heightmapUVScale.x;
    float texelV    = kStencilTexels * g_heightmapTexel * g_heightmapUVScale.y;
    float worldStep = kStencilTexels * g_heightmapTexel * g_worldSize;

    float hL = HeightAt(hmUV + float2(-texelU, 0));
    float hR = HeightAt(hmUV + float2( texelU, 0));
    float hD = HeightAt(hmUV + float2(0, -texelV));
    float hU = HeightAt(hmUV + float2(0,  texelV));

    // Surface y = h(x,z). Centred-difference gradients then build the
    // standard right-handed TBN frame.
    N = normalize(float3(hL - hR, 2.0 * worldStep, hD - hU));
    T = normalize(float3(2.0 * worldStep, hR - hL, 0.0));
    B = normalize(cross(T, N));     // T × N points along +Z (matches +V) for DX-style normal maps
}

// Note: GBuffer.ps.hlsl writes the world normal as `n * 0.5 + 0.5` with
// all three channels packed into the R16G16B16A16_FLOAT normal RT (NOT
// octahedral). Terrain follows the same convention so LightingPass can
// decode it identically.

// ---- Per-map samplers -----------------------------------------------------
//
// Albedo + normal use the hex-grid stochastic 3-tap (TerrainStochasticTaps
// in Terrain.hlsli) — this is what removes the visible row/column
// repetition. ARM + disp are 1-tap because they're low-frequency content
// where the lattice isn't visible and the extra samples would just burn
// bandwidth.
float3 SampleAlbedo(int idx, float2 uv)
{
    if (idx < 0) return 0.0;
    float2 uv0, uv1, uv2;
    float  w0,  w1,  w2;
    float2 r0, r1, r2;          // rotation cos/sin per tap (unused for albedo)
    TerrainStochasticTaps(uv, uv0, uv1, uv2, w0, w1, w2, r0, r1, r2);
    return g_AllTextures[idx].Sample(g_LinearWrap, uv0).rgb * w0
         + g_AllTextures[idx].Sample(g_LinearWrap, uv1).rgb * w1
         + g_AllTextures[idx].Sample(g_LinearWrap, uv2).rgb * w2;
}

// DX-style normal map (Y-up). Returns a tangent-space normal in [-1,1].
// Falls back to (0,0,1) for missing maps so the macro normal passes through.
//
// Each stochastic tap reads the texture from a UV that's been rotated by
// rotN; the sampled tangent-space normal is therefore expressed in a
// tangent frame that's also rotated by rotN. To blend the three taps in
// the SAME tangent frame, back-rotate each sampled normal's (X, Y) by
// -rotN. The Z component (out of the surface) is invariant.
float3 SampleNormalTS(int idx, float2 uv)
{
    if (idx < 0) return float3(0, 0, 1);
    float2 uv0, uv1, uv2;
    float  w0,  w1,  w2;
    float2 r0, r1, r2;
    TerrainStochasticTaps(uv, uv0, uv1, uv2, w0, w1, w2, r0, r1, r2);

    float3 n0 = g_AllTextures[idx].Sample(g_LinearWrap, uv0).rgb * 2.0 - 1.0;
    float3 n1 = g_AllTextures[idx].Sample(g_LinearWrap, uv1).rgb * 2.0 - 1.0;
    float3 n2 = g_AllTextures[idx].Sample(g_LinearWrap, uv2).rgb * 2.0 - 1.0;

    // Back-rotate by -angle: (cos -sin; sin cos)^-1 = (cos sin; -sin cos)
    n0.xy = float2( r0.x * n0.x + r0.y * n0.y,
                   -r0.y * n0.x + r0.x * n0.y);
    n1.xy = float2( r1.x * n1.x + r1.y * n1.y,
                   -r1.y * n1.x + r1.x * n1.y);
    n2.xy = float2( r2.x * n2.x + r2.y * n2.y,
                   -r2.y * n2.x + r2.x * n2.y);

    // Linear weighted sum on tangent-space normals — caller normalizes
    // after blending across all 4 layers, so we don't normalize per-layer.
    return n0 * w0 + n1 * w1 + n2 * w2;
}

// ARM: R = AO, G = Roughness, B = Metalness. Defaults give matte dielectric.
float3 SampleARM(int idx, float2 uv)
{
    if (idx < 0) return float3(1.0, 0.85, 0.0);
    return g_AllTextures[idx].Sample(g_LinearWrap, uv).rgb;
}

// Single-channel displacement (height) for height-correlated blending.
// 1-tap (low-frequency); missing map → 0 (no bias for that layer).
float SampleDisp(int idx, float2 uv)
{
    if (idx < 0) return 0.0;
    return g_AllTextures[idx].Sample(g_LinearWrap, uv).r;
}

// ---- Per-layer auto-blend weight -------------------------------------------
// Layer i is fully visible where worldY ∈ [minHeight, maxHeight] AND the
// surface slope (0=flat, 90°=cliff) ∈ [minSlope, maxSlope]; the two gates
// multiply and each end smoothstep-fades over its fade width.
//
//   minHeight     maxHeight
//        ▼            ▼
//   ┌────┬────────────┬────┐
//   │fade│   weight=1 │fade│
//   └────┴────────────┴────┘
float AutoBlendWeight(TerrainLayerGPU L, float worldY, float slopeDeg)
{
    const float hFade = max(L.fadeHeight, 1e-3);
    const float hLo = smoothstep(L.minHeight - hFade, L.minHeight, worldY);
    const float hHi = 1.0 - smoothstep(L.maxHeight, L.maxHeight + hFade, worldY);
    const float wH  = saturate(hLo * hHi);

    const float sFade = max(L.fadeSlopeDeg, 1e-3);
    const float sLo = smoothstep(L.minSlopeDeg - sFade, L.minSlopeDeg, slopeDeg);
    const float sHi = 1.0 - smoothstep(L.maxSlopeDeg, L.maxSlopeDeg + sFade, slopeDeg);
    const float wS  = saturate(sLo * sHi);

    return wH * wS;
}

GOut main(PSIn i)
{
    // ---- Per-pixel geometric TBN (heightmap-derived, smooth) --------------
    float3 N_geom, T_geom, B_geom;
    ComputeTerrainTBN(i.worldPos, N_geom, T_geom, B_geom);

    // ---- Determine which layers exist + the blend source -------------------
    const uint n = min(g_layerCount, (uint)MAX_TERRAIN_LAYERS);

    bool anyLayer = false;
    for (uint a = 0; a < n; ++a)
        if (g_TerrainLayers[a].albedoIdx >= 0) { anyLayer = true; break; }

    // Splatmap (authored weights) — only meaningful for the first 4 layers
    // (RGBA). When unset, each layer's weight comes from its height/slope gate.
    float4 splat4 = 0;
    if (g_hasSplatmap != 0)
        splat4 = g_Splatmap.Sample(g_LinearClamp, i.uv);

    if (!(g_hasSplatmap != 0 || anyLayer))
    {
        // Final fallback: slope-debug palette when neither splatmap nor any
        // layer is wired up (Phase 1 behaviour).
        float slope = saturate(1.0 - N_geom.y);
        float3 grass = float3(0.30, 0.46, 0.22);
        float3 rock  = float3(0.45, 0.42, 0.40);
        GOut o;
        o.albedo  = float4(lerp(grass, rock, smoothstep(0.30, 0.65, slope)), 1.0);
        // Engine convention (GBuffer.ps.hlsl line 215): pack world normal as
        // n * 0.5 + 0.5 in RGB; .a = matIdx (0 = first material slot, no
        // SSAO-exclusion sign bit since matIdx 0 ≥ 0).
        o.normal  = float4(N_geom * 0.5 + 0.5, 0.0);
        o.surface = float4(0.85, 0.0, 1.0, 0.5);
        // Raw NDC delta — matches GBuffer.ps; decoders apply (0.5,-0.5).
        float2 cur  = i.curClip.xy  / i.curClip.w;
        float2 prev = i.prevClip.xy / i.prevClip.w;
        o.velocity = cur - prev;
        o.extra    = float4(0, 0, 0, 0);
        o.sceneCol = float4(0, 0, 0, 1);   // terrain has no emissive
        return o;
    }

    // ---- Per-layer blend (count-driven, optional height-correlation) -------
    // Pass 1 builds each layer's base weight (splatmap RGBA or height/slope
    // gate). When g_heightBlendEnable is on, the disp map biases that weight
    // (v = w + disp·strength) and only layers within g_heightBlendRange of the
    // local maximum survive — so transitions follow the micro-relief instead of
    // a flat lerp. Pass 2 samples + accumulates with the final weights.
    // Linear blend = Σ(sample·w) / Σw, so we accumulate unnormalised and divide
    // once at the end (the normal sum is normalised directly).
    const float  worldY   = i.worldPos.y;
    // Slope from per-pixel geometric normal — 0 (flat) to 90 (cliff).
    const float  slopeDeg = degrees(acos(saturate(N_geom.y)));
    const float2 worldXZ  = i.worldPos.xz;

    float baseW[MAX_TERRAIN_LAYERS];
    float vBias[MAX_TERRAIN_LAYERS];
    float vMax = -1e9;

    [loop] for (uint li = 0; li < n; ++li)
    {
        TerrainLayerGPU L = g_TerrainLayers[li];

        float w = (g_hasSplatmap != 0) ? ((li < 4) ? splat4[li] : 0.0)
                                       : AutoBlendWeight(L, worldY, slopeDeg);
        // Mask disabled layers (no albedo bound) so the blend stays normalised.
        w *= (L.albedoIdx >= 0) ? 1.0 : 0.0;
        baseW[li] = w;

        // Displacement bias (only for active layers — disp must never lift a
        // layer the splat/gate already zeroed). Disabled layers are pushed far
        // below the max so they never survive the height-blend cutoff.
        float v = -1e9;
        if (w > 0.0)
        {
            v = w;
            if (g_heightBlendEnable != 0)
            {
                float disp = SampleDisp(L.dispIdx, worldXZ * L.tilingScale);
                v = w + disp * g_heightBlendStrength;
            }
            vMax = max(vMax, v);
        }
        vBias[li] = v;
    }

    float3 albedoAccum = 0;
    float3 nTSAccum    = 0;
    float3 armAccum    = 0;
    float  wSum        = 0;

    [loop] for (uint li = 0; li < n; ++li)
    {
        if (baseW[li] <= 0.0) continue;

        // Height-correlated cutoff vs plain linear weight.
        float w = (g_heightBlendEnable != 0)
                ? max(0.0, vBias[li] - vMax + g_heightBlendRange)
                : baseW[li];
        if (w <= 0.0) continue;

        TerrainLayerGPU L = g_TerrainLayers[li];
        float2 uv = worldXZ * L.tilingScale;
        wSum += w;
#if TERRAIN_DEBUG_SPLAT
        // Debug: weight → primary colour (0=R, 1=G, 2=B, 3=Y, ≥4=white).
        float3 dbg = (li == 0) ? float3(1, 0, 0)
                   : (li == 1) ? float3(0, 1, 0)
                   : (li == 2) ? float3(0, 0, 1)
                   : (li == 3) ? float3(1, 1, 0)
                               : float3(1, 1, 1);
        albedoAccum += dbg * w;
#else
        albedoAccum += SampleAlbedo(L.albedoIdx, uv) * w;
#endif
        nTSAccum += SampleNormalTS(L.normalIdx, uv) * w;
        armAccum += SampleARM(L.armIdx, uv) * w;
    }

    const float invW = (wSum > 1e-4) ? rcp(wSum) : 0.0;

    // ---- GBuffer output ---------------------------------------------------
    GOut o;
    o.albedo = float4(albedoAccum * invW, 1.0);

    // Per-pixel TBN built from heightmap derivatives — see ComputeTerrainTBN.
    float3 nTS     = (wSum > 1e-4) ? normalize(nTSAccum) : float3(0, 0, 1);
    float3 N_world = normalize(nTS.x * T_geom + nTS.y * B_geom + nTS.z * N_geom);
    // Engine convention (GBuffer.ps.hlsl line 215): world normal packed as
    // n * 0.5 + 0.5 in RGB; .a = matIdx with sign bit reserved for the
    // SSAO-exclusion flag. Terrain uses matIdx = 0 (no per-material data).
    o.normal = float4(N_world * 0.5 + 0.5, 0.0);

    // Surface RT layout: R=Roughness, G=Metalness, B=AO, A=Reflectance.
    float3 arm = armAccum * invW;
    o.surface = float4(arm.g, arm.b, arm.r, 0.5);

    // Raw NDC delta — matches GBuffer.ps; decoders apply (0.5,-0.5).
    float2 cur  = i.curClip.xy  / i.curClip.w;
    float2 prev = i.prevClip.xy / i.prevClip.w;
    o.velocity = cur - prev;

    o.extra    = float4(0, 0, 0, 0);
    o.sceneCol = float4(0, 0, 0, 1);   // terrain has no emissive
    return o;
}
