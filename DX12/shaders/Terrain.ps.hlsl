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

    int4   g_layerBindlessIdx;
    float4 g_layerTilingScale;

    int4   g_layerNormalIdx;
    int4   g_layerARMIdx;
    int4   g_layerDispIdx;

    uint   g_tilesPerSide;
    uint   _g_enableFrustumCull;   // AS-only — kept for layout parity
    float  _g_pad8a;
    float  _g_pad8b;

    // Per-layer auto-blend (no splatmap path). World-meter heights, degrees
    // for slopes. Layer i is fully visible when worldY ∈ [min,max] AND the
    // surface slope (0=flat, 90°=cliff) ∈ [minSlope,maxSlope]; weights
    // smoothstep down to 0 within the fade widths on both sides.
    float4 g_layerMinHeight;
    float4 g_layerMaxHeight;
    float4 g_layerFadeHeight;
    float4 g_layerMinSlopeDeg;
    float4 g_layerMaxSlopeDeg;
    float4 g_layerFadeSlopeDeg;

    float4 _g_frustumPlanes[6];    // AS-only — kept for layout parity
};

// Heightmap is bound at t2 space0 (descriptor-table SRV with ALL stage
// visibility), so the PS can re-sample it for per-pixel analytic normal.
Texture2D<float>  g_HeightMap    : register(t2, space0);
Texture2D<float4> g_Splatmap     : register(t3, space0);
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

float SampleDisp(int idx, float2 uv)
{
    if (idx < 0) return 0.5;            // neutral height when missing
    return g_AllTextures[idx].Sample(g_LinearWrap, uv).r;
}

// ---- Height-correlated splat blend ----------------------------------------
// Input: 4 splat weights + 4 displacement samples.
// The disp samples bias the blend so layer transitions follow the underlying
// micro-relief instead of being a flat lerp ("grass tufts poke through where
// the gravel dips"). Two parameters control how much they matter:
//
//   heightStrength — multiplier on disp before adding to splat. Small
//                    (0.10–0.20) keeps splat as the dominant signal.
//                    Large (0.5+) lets disp override splat → only the
//                    layer with highest disp wins, which is the bug we
//                    just fixed.
//   blendRange     — soft cutoff width. A layer keeps non-zero weight
//                    while v_i ≥ v_max − blendRange. Wider = smoother
//                    multi-layer blends; narrower = sharper transitions.
float4 HeightBlend(float4 splat, float4 heights, float heightStrength, float blendRange)
{
    float4 v = splat + heights * heightStrength;
    float  m = max(max(v.x, v.y), max(v.z, v.w));
    float4 w = max(0.0, v - m + blendRange);
    return w;
}

GOut main(PSIn i)
{
    // ---- Per-pixel geometric TBN (heightmap-derived, smooth) --------------
    float3 N_geom, T_geom, B_geom;
    ComputeTerrainTBN(i.worldPos, N_geom, T_geom, B_geom);

    // ---- Splat weights (authored splatmap OR auto-blend by altitude+slope)
    bool anyLayer =
        (g_layerBindlessIdx.x >= 0) ||
        (g_layerBindlessIdx.y >= 0) ||
        (g_layerBindlessIdx.z >= 0) ||
        (g_layerBindlessIdx.w >= 0);

    float4 splat = 0;
    if (g_hasSplatmap != 0)
    {
        splat = g_Splatmap.Sample(g_LinearClamp, i.uv);
    }
    else if (anyLayer)
    {
        // Per-layer auto-blend: each layer is gated by an INTUITIVE pair of
        // ranges authored in the inspector — world-meter height range and
        // degree slope range. The two gates multiply, so a layer only shows
        // where BOTH conditions hold. Soft falloffs on each end use
        // smoothstep so transitions are C1-smooth.
        //
        //   minHeight     maxHeight
        //        ▼            ▼
        //   ┌────┬────────────┬────┐
        //   │fade│   weight=1 │fade│
        //   └────┴────────────┴────┘
        //
        // Same shape applies for slope. Both ranges are per-layer, so one
        // layer can be "tight band, sharp edges" while another is "broad
        // band, soft edges" without any global tuning knob.
        const float worldY = i.worldPos.y;
        // Slope from per-pixel geometric normal — 0 (flat) to 90 (cliff).
        // acos clamped via saturate to keep numerical noise from going past 1.
        const float slopeDeg = degrees(acos(saturate(N_geom.y)));

        [unroll] for (int li = 0; li < 4; ++li)
        {
            const float hMin  = g_layerMinHeight   [li];
            const float hMax  = g_layerMaxHeight   [li];
            const float hFade = max(g_layerFadeHeight[li], 1e-3);
            // Height weight — 1 inside [hMin, hMax], smoothstep falloff
            // [hMin-hFade, hMin] up and [hMax, hMax+hFade] down.
            const float hLo = smoothstep(hMin - hFade, hMin, worldY);
            const float hHi = 1.0 - smoothstep(hMax, hMax + hFade, worldY);
            const float wH  = saturate(hLo * hHi);

            const float sMin  = g_layerMinSlopeDeg   [li];
            const float sMax  = g_layerMaxSlopeDeg   [li];
            const float sFade = max(g_layerFadeSlopeDeg[li], 1e-3);
            const float sLo = smoothstep(sMin - sFade, sMin, slopeDeg);
            const float sHi = 1.0 - smoothstep(sMax, sMax + sFade, slopeDeg);
            const float wS  = saturate(sLo * sHi);

            splat[li] = wH * wS;
        }
    }

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
        float2 cur  = i.curClip.xy  / i.curClip.w;
        float2 prev = i.prevClip.xy / i.prevClip.w;
        o.velocity = (cur - prev) * float2(0.5, -0.5);
        o.extra    = float4(0, 0, 0, 0);
        o.sceneCol = float4(0, 0, 0, 1);   // terrain has no emissive
        return o;
    }

    // Mask out disabled layers so blend stays normalised.
    splat.x *= (g_layerBindlessIdx.x >= 0) ? 1.0 : 0.0;
    splat.y *= (g_layerBindlessIdx.y >= 0) ? 1.0 : 0.0;
    splat.z *= (g_layerBindlessIdx.z >= 0) ? 1.0 : 0.0;
    splat.w *= (g_layerBindlessIdx.w >= 0) ? 1.0 : 0.0;

    // ---- Per-layer UVs (worldXZ × per-layer tilingScale) ------------------
    float2 worldXZ = i.worldPos.xz;
    float2 uv0 = worldXZ * g_layerTilingScale.x;
    float2 uv1 = worldXZ * g_layerTilingScale.y;
    float2 uv2 = worldXZ * g_layerTilingScale.z;
    float2 uv3 = worldXZ * g_layerTilingScale.w;

    // ---- Linear splat blend (no height-blend) -----------------------------
    // The previous height-correlated blend ended up zeroing layers that the
    // splat had already weighted ≥ 0 — it's much easier to debug a pure
    // splat blend first, then layer height-blending back on top once we
    // know the splat itself reaches all four layers.
    float4 w = splat;

    float wSum = w.x + w.y + w.z + w.w;
    if (wSum > 1e-4) w *= rcp(wSum);

#if TERRAIN_DEBUG_SPLAT
    // Debug: splat weights → primary colours.
    //   layer 0 (grass)   → RED
    //   layer 1 (rocks)   → GREEN
    //   layer 2 (pebbles) → BLUE
    //   layer 3 (stone)   → YELLOW (R+G)
    float3 albedoColor = w.x * float3(1, 0, 0)
                       + w.y * float3(0, 1, 0)
                       + w.z * float3(0, 0, 1)
                       + w.w * float3(1, 1, 0);
#else
    // ---- Albedo blend -----------------------------------------------------
    float3 albedoColor = SampleAlbedo(g_layerBindlessIdx.x, uv0) * w.x
                      + SampleAlbedo(g_layerBindlessIdx.y, uv1) * w.y
                      + SampleAlbedo(g_layerBindlessIdx.z, uv2) * w.z
                      + SampleAlbedo(g_layerBindlessIdx.w, uv3) * w.w;
#endif

    // ---- Normal blend (tangent-space sum, then TBN-transform) -------------
    float3 nTS = SampleNormalTS(g_layerNormalIdx.x, uv0) * w.x
              + SampleNormalTS(g_layerNormalIdx.y, uv1) * w.y
              + SampleNormalTS(g_layerNormalIdx.z, uv2) * w.z
              + SampleNormalTS(g_layerNormalIdx.w, uv3) * w.w;
    nTS = normalize(nTS);

    // Per-pixel TBN built from heightmap derivatives — see ComputeTerrainTBN.
    float3 N_world = normalize(nTS.x * T_geom + nTS.y * B_geom + nTS.z * N_geom);

    // ---- ARM blend (AO / Roughness / Metalness) ---------------------------
    float3 arm = SampleARM(g_layerARMIdx.x, uv0) * w.x
              +  SampleARM(g_layerARMIdx.y, uv1) * w.y
              +  SampleARM(g_layerARMIdx.z, uv2) * w.z
              +  SampleARM(g_layerARMIdx.w, uv3) * w.w;

    // ---- GBuffer output ---------------------------------------------------
    GOut o;
    o.albedo  = float4(albedoColor, 1.0);
    // Engine convention (GBuffer.ps.hlsl line 215): world normal packed as
    // n * 0.5 + 0.5 in RGB; .a = matIdx with sign bit reserved for the
    // SSAO-exclusion flag. Terrain uses matIdx = 0 (no per-material data).
    o.normal  = float4(N_world * 0.5 + 0.5, 0.0);
    // Surface RT layout: R=Roughness, G=Metalness, B=AO, A=Reflectance.
    o.surface = float4(arm.g, arm.b, arm.r, 0.5);

    float2 cur  = i.curClip.xy  / i.curClip.w;
    float2 prev = i.prevClip.xy / i.prevClip.w;
    o.velocity = (cur - prev) * float2(0.5, -0.5);

    o.extra    = float4(0, 0, 0, 0);
    o.sceneCol = float4(0, 0, 0, 1);   // terrain has no emissive
    return o;
}
