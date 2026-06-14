// Water.ps.hlsl — forward water surface: Fresnel sky reflection + flow normals.
//
// Deliberately lightweight (per design): no refraction copy, no SSR — the
// surface is a Fresnel blend between an absorption-tinted body colour and
// the sky cubemap (SkyIBLPass atmosphere cube or the static skybox), plus a
// Blinn specular sun glint. "Flow" = TWO tangent-space normal maps counter-
// scrolling along g_flowDir (A = broad swell at g_normalTiling, B = fine
// detail at 2.3× tiling against the flow), whiteout-blended. Maps come from
// WaterComponent::normalMapA/BPath (editor drag-drop); a 1×1 flat-normal
// fallback gives calm water while they load.
//
// Water depth is computed ANALYTICALLY from the terrain heightmap (water
// plane Y − terrain Y at the pixel's XZ) — no scene-depth SRV needed, so the
// pass can hardware-depth-test against the GBuffer depth at the same time
// (the engine has no read-only DSV; see TracerPass for the SRV alternative).
//
// Bindings: b1 PerView (VS), b2 WaterCB, b3 LightCB,
//           t2 space0 terrain heightmap, t3 space0 sky TextureCube,
//           t4/t5 space0 flow-normal maps A/B, s0 clamp, s1 aniso-wrap.

#define LIGHT_CB_REGISTER b3
#include "light_cb.hlsli"
#include "Terrain.hlsli"   // TerrainSampleHeightBicubic — must match Terrain.ms

// CSM shadow sampling — the sun glint must vanish when terrain occludes the
// sun. shadow.hlsli needs these declared first (all CB fields come from
// light_cb.hlsli above).
Texture2DArray<float>  gShadowCascades : register(t9, space0);
SamplerComparisonState gShadowSampler  : register(s2, space0);
#include "shadow.hlsli"

cbuffer WaterCB : register(b2, space0)
{
    float2 g_waterOrigin;
    float  g_waterSize;
    float  g_waterHeight;

    float4 g_deepColor;
    float4 g_shallowColor;

    float2 g_flowDir;
    float  g_flowSpeed;
    float  g_normalTiling;

    float  g_normalStrength;
    float  g_absorbDist;
    float  g_shoreFade;
    float  g_fresnelF0;

    float  g_specPower;
    float  g_reflStrength;
    float  g_waterTime;
    uint   g_gridQuads;

    float2 g_terrainOrigin;
    float  g_terrainSize;
    float  g_terrainBaseY;

    float  g_terrainHeightScale;
    uint   g_hasTerrain;
    float  g_hmTexel;
    float  g_waterRoughness;   // written to GBuffer2.r — drives the SSR cone

    float2 g_hmUVOffset;
    float2 g_hmUVScale;
};

Texture2D<float> g_HeightMap : register(t2,  space0);
TextureCube      g_SkyCube   : register(t3,  space0);
Texture2D        g_NormalA   : register(t4,  space0);
Texture2D        g_NormalB   : register(t5,  space0);
// Prev-frame SSR result (rgb = reflection color, a = confidence) — same
// 1-frame-latent convention as Lighting.ps's gSSRResult. Used only to
// dampen the sky term; SSRComposite adds ssr×F×conf this frame.
Texture2D<float4> g_SSRResult : register(t27, space0);
SamplerState     g_Clamp     : register(s0,  space0);
SamplerState     g_Wrap      : register(s1,  space0);

struct PSIn
{
    float4 sv       : SV_Position;
    float3 worldPos : POSITIONWS;
    float4 curClip  : TEXCOORD0;
    float4 prevClip : TEXCOORD1;
};

struct PSOut
{
    float4 normal   : SV_TARGET0;  // GBuffer1: N*0.5+0.5, a=0 — SSR trace input
    float4 surface  : SV_TARGET1;  // GBuffer2: (rough, metal, AO, 0.5)
    float2 velocity : SV_TARGET2;  // GBuffer3: NDC delta — TAA/SSR temporal
    float4 color    : SV_TARGET3;  // HDR scene color (alpha-blended shoreline)
};

// ---- Flow normals from the two authored normal maps -------------------------
// Map A scrolls WITH the flow at the base tiling (broad swell); map B scrolls
// AGAINST it at 2.3× tiling (fine detail). Whiteout blend in tangent space,
// then tangent→world for a +Y plane (U → +X, V → +Z): world = (ts.x, ts.z, ts.y).

float3 WaterFlowNormal(float2 xz)
{
    float2 flowOff = g_flowDir * (g_waterTime * g_flowSpeed);

    float2 uvA = (xz - flowOff)        * g_normalTiling;
    float2 uvB = (xz + flowOff * 0.62) * (g_normalTiling * 2.3) + 0.37;

    // Sample RG only and reconstruct Z — the importer packs normal maps as
    // BC5 (two-channel), same convention as GBuffer.ps / Transparent.ps.
    // Works identically for RGBA fallbacks (rg = 0.5,0.5 → z = 1).
    float2 sA = g_NormalA.Sample(g_Wrap, uvA).rg * 2.0 - 1.0;
    float2 sB = g_NormalB.Sample(g_Wrap, uvB).rg * 2.0 - 1.0;
    float  zA = sqrt(saturate(1.0 - dot(sA, sA)));
    float  zB = sqrt(saturate(1.0 - dot(sB, sB)));

    // Whiteout blend; g_normalStrength scales the combined slope.
    float2 slope = (sA + sB) * g_normalStrength;
    float  up    = max(zA * zB, 0.05);

    return normalize(float3(slope.x, up, slope.y));
}

float TerrainYAt(float2 xz)
{
    float2 norm = (xz - g_terrainOrigin) / max(g_terrainSize, 1e-3);
    if (g_hasTerrain == 0 ||
        norm.x < 0.0 || norm.x > 1.0 || norm.y < 0.0 || norm.y > 1.0)
        return g_waterHeight - g_absorbDist * 4.0;   // off-tile → deep
    float2 uv = g_hmUVOffset + norm * g_hmUVScale;
    // Bicubic — must match Terrain.ms.hlsl's rasterized surface exactly, or
    // the shoreline alpha fades out where the rendered terrain is still
    // underwater (the two filters disagree by ~secondDifference/6).
    float  h  = TerrainSampleHeightBicubic(g_HeightMap, g_Clamp, uv, g_hmTexel);
    return g_terrainBaseY + h * g_terrainHeightScale;
}

PSOut main(PSIn i)
{
    const float3 wp = i.worldPos;
    const float3 V  = normalize(cameraPos - wp);

    const bool underwater = (V.y < 0.0);          // camera under the plane
    float3 N = WaterFlowNormal(wp.xz);
    if (underwater) N = float3(N.x, -N.y, N.z);

    // ---- Analytic water depth → absorption + shoreline fade -----------------
    const float depth   = max(g_waterHeight - TerrainYAt(wp.xz), 0.0);
    const float absorb  = 1.0 - exp(-depth / max(g_absorbDist, 1e-3));
    float3 body = lerp(g_shallowColor.rgb, g_deepColor.rgb, absorb);
    const float alpha = saturate(depth / max(g_shoreFade, 1e-3));

    // Fully-faded shoreline pixels: discard so they don't stamp depth or
    // GBuffer normals (terrain stays authoritative right at the waterline).
    clip(alpha - 0.02);

    // ---- Sun visibility (CSM) -------------------------------------------------
    // Computed up front — used by both the analytic glint below AND the
    // sun-aligned occlusion of the cube reflection. Bias normal = plane up
    // (the flow normal is too high-frequency for the receiver offset).
    const float  sunShadow = ComputeShadowFactor(wp, float3(0, 1, 0), i.sv.xy);
    const float3 L = normalize(-lightDir);   // FROM-light convention → negate

    // ---- Fresnel sky reflection ----------------------------------------------
    const float NdotV = saturate(dot(N, V));
    const float F = g_fresnelF0 + (1.0 - g_fresnelF0) * pow(1.0 - NdotV, 5.0);

    float3 R = reflect(-V, N);
    float3 sky;
    if (underwater)
    {
        // Looking up at the underside: the reflected ray points back into the
        // water body (TIR-ish look) — the sky cube's below-horizon content is
        // undefined, so use the absorption colour as the reflection term.
        sky = g_deepColor.rgb * g_reflStrength;
    }
    else
    {
        R.y = max(R.y, 0.02);                      // keep off the lower hemisphere
        R   = normalize(R);
        sky = g_SkyCube.SampleLevel(g_Clamp, R, 0).rgb * g_reflStrength;

        // An environment cube can't be occluded by scene geometry — at a
        // low sun the long bright streak (the cube's sun + halo) crosses
        // water the terrain has already shadowed. For the sun-aligned part
        // of the reflection the light path eye→surface→sun IS the path the
        // CSM measures, so the receiver's sun visibility applies exactly.
        // The pow width covers the sun halo; widen (lower exponent) if the
        // streak still leaks, tighten if shadows darken too much sky.
        const float sunAlign = pow(saturate(dot(R, L)), 8.0);
        sky *= lerp(1.0, sunShadow, sunAlign);
    }

    // ---- SSR dampening (prev frame, same convention as Lighting.ps) ----------
    // SSRComposite adds ssr.rgb × F × envBRDF × conf onto the HDR later this
    // frame, using the normal/roughness this PS writes to the GBuffer below.
    //
    // The sky term must make room — but a LINEAR (1 − conf) residual visibly
    // washes out the SSR reflection: confidence rarely reaches 1 (temporal
    // ramp, spatial reweight) while the sky is often 10–100× brighter than
    // the reflected scene, so even 20–30% sky bleed drowns a dark terrain
    // reflection (and scales with g_reflStrength). Treat a moderately
    // confident hit as fully occluding the sky: smoothstep keeps the soft
    // fade near conf≈0 (true misses) but removes the bleed on solid hits.
    //
    // t27 holds the PREVIOUS frame's result, laid out on the previous
    // camera's pixel grid. Loading it at the current raster coordinate
    // misaligns the confidence mask by the per-frame camera delta — under
    // motion that painted a band along every confidence gradient (sky
    // removed where no reflection lands, or both stacked). Sample at this
    // surface point's PREV-frame UV instead: prevClip is already computed
    // for velocity, and at the contact line the surface and its reflection
    // share a depth, so surface reprojection is exact right where the
    // artifact lives. Off-screen prev → fall back to the current pixel.
    float ssrConf = 0.0;
    if (!underwater && i.prevClip.w > 1e-3)
    {
        float2 prevNdc = i.prevClip.xy / i.prevClip.w;
        float2 prevUV  = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
        ssrConf = all(saturate(prevUV) == prevUV)
            ? saturate(g_SSRResult.SampleLevel(g_Clamp, prevUV, 0).a)
            : saturate(g_SSRResult.Load(int3(int2(i.sv.xy), 0)).a);
    }
    const float skyVis = 1.0 - smoothstep(0.05, 0.55, ssrConf);
    sky *= skyVis;

    // ---- Sun glint (Blinn, Fresnel-weighted, CSM-shadowed) --------------------
    // Direct sunlight — multiplied by the CSM factor computed above so the
    // glint vanishes where terrain occludes the sun.
    const float3 H = normalize(L + V);
    const float  NdotH = saturate(dot(N, H));
    const float  VdotH = saturate(dot(V, H));
    const float  Fspec = g_fresnelF0 + (1.0 - g_fresnelF0) * pow(1.0 - VdotH, 5.0);
    float3 spec = lightColor * (pow(NdotH, g_specPower)
                  * Fspec * (g_specPower + 8.0) * (1.0 / 25.13))
                  * sunShadow;

    float3 color = lerp(body, sky, F) + spec;

    PSOut o;
    // GBuffer stamps for the post-frame SSR trace: animated flow normal +
    // mirror-ish roughness. surface.a = 0.5 → composite F0 = 0.04 (water).
    o.normal  = float4(N * 0.5 + 0.5, 0.0);
    o.surface = float4(g_waterRoughness, 0.0, 1.0, 0.5);

    // Motion vectors so TAA and the SSR temporal reproject water pixels with
    // the WATER's motion, not the submerged terrain's / shoreline grass's.
    // Encode is the RAW NDC delta (curNDC - prevNDC) — the convention
    // GBuffer.ps writes and every decoder assumes (TAA_Reproject.hlsli
    // prevNDC = curNDC - velocity; SSRTemporal/XeGTAOTemporal apply the
    // (0.5,-0.5) NDC→UV scale themselves). Pre-scaling by (0.5,-0.5) here
    // would halve X and FLIP Y after decode.
    float2 cur  = i.curClip.xy  / i.curClip.w;
    float2 prev = i.prevClip.xy / i.prevClip.w;
    o.velocity = cur - prev;

    o.color   = float4(color, alpha);
    return o;
}
