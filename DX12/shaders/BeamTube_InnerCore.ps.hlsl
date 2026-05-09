// BeamTube_InnerCore.ps.hlsl — Opaque GBuffer custom PS for the inner core
// of a procedural-tube heavy beam. Drop-in for MaterialComponent.customShaderPath
// with userBlendMode = Opaque.
//
// Drives shading from the bindless material slot (MaterialGPUData) plus a
// dedicated custom CBV that the engine reflects into the Inspector.
//
//   BeamColor       — linear HDR core colour (1..10)
//   Intensity       — multiplier on top of BeamColor.rgb (1..20)
//   HotCoreColor    — lerp target for the hottest centre (white-hot tip)
//   RimPower        — fresnel rim sharpness exponent
//   RimIntensity    — additional emissive at glancing angles
//   NoiseTiling     — number of noise tiles along the beam (uv.y)
//   ScrollSpeed     — noise UV per second
//   NoiseFloor      — minimum noise multiplier (avoids dark gaps)
//   Roughness       — surface roughness (drives IBL spec sharpness)
//
// Output:
//   albedo  → small physical baseColor + LARGE emissive baked in (HDR)
//   normal  → real tube normal (drives Lighting/IBL)
//   surface → roughness/metalness/AO/reflectance
//   velocity → standard TAA motion vector

#include "material.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

StructuredBuffer<MaterialGPUData> g_Materials   : register(t2, space0);
Texture2D                         g_AllTextures[] : register(t0, space2);
SamplerState                      g_LinearWrap   : register(s0, space0);

// User-tunable params (Inspector picks these up via reflection).
cbuffer BeamCoreParams : register(b8, space0)
{
    float4 BeamColor;      // .rgb HDR core; .a unused
    float  Intensity;
    float  RimPower;
    float  RimIntensity;
    float  NoiseTiling;
    float  ScrollSpeed;
    float  NoiseFloor;
    float  Roughness;
    float  _pad0;
    float4 HotCoreColor;   // hottest centre tint (lerped in via radial term)
};

// Optional noise texture (drag into the material's DetailTex slot in Inspector).
Texture2D NoiseTex : register(t0, space3);

struct PSIn
{
    float4 sv        : SV_POSITION;
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
    float4 extra    : SV_TARGET4;
    float4 sceneCol : SV_TARGET5;   // HdrSceneColor: emissive seed
};

// PerView CB carries cameraPos in matrix _41/_42/_43 — but easier to use the
// engine's existing camera reconstruction: read from material baseColor
// fallback when CB unbound (this shader will only ever run with a custom
// CBV, so `Intensity` etc. are always valid).

float Hash12(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float ProceduralNoise1D(float u, float scrollOffset)
{
    float fp = u + scrollOffset;
    float i  = floor(fp);
    float f  = frac(fp);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(Hash12(float2(i, 7.0)), Hash12(float2(i + 1.0, 7.0)), f);
}

GOut main(PSIn i)
{
    MaterialGPUData mat = g_Materials[materialIndex];

    // ---- View vector for fresnel rim ---------------------------------------
    // The engine doesn't pass cameraPos in PushConstants for GBuffer PS, so
    // approximate from the eye-space normal: rim factor = 1 - |N·V|. Without
    // explicit V, derive a pseudo-view by treating worldPos as relative to
    // the average reflection probe origin. Simpler approach used here: just
    // compute the screen-space gradient of position to recover -V direction
    // — but that's expensive. Better: pass cameraPos via the bindless mat
    // header. For MVP, fall back to a UV-based "edge" term that approximates
    // a fresnel rim: pixels with uv.x close to 0 or 1 (tube horizon) get
    // brighter rim. uv.x is 0..1 around the ring; horizon at side of camera.
    // This is a reasonable proxy because the cylindrical billboard ensures
    // uv.x distance from 0.5 correlates with the silhouette.
    float side = abs(i.uv.x - 0.5) * 2.0;       // 0 centre, 1 silhouette
    float rim  = pow(saturate(side), max(RimPower, 0.001));

    // ---- Scrolling noise along the beam axis -------------------------------
    // Use mat-driven time isn't available; the global engine time isn't in
    // PushConstants either. The CB-based ScrollSpeed is in noise-units per
    // second, so we'd need time in the CBV. For MVP we treat ScrollSpeed as
    // a static pattern offset (no animation) — the OuterGlow shader handles
    // the animated scroll. Future enhancement: pipe globalTime into the
    // BeamCoreParams CBV via Renderer (similar to BeamGenParamsGPU.time).
    float u = i.uv.y * max(NoiseTiling, 0.001) + ScrollSpeed * 0.0;
    float n;
    if (any(NoiseTex.SampleLevel(g_LinearWrap, float2(0.5, 0.5), 0).rgb))
    {
        // NoiseTex is non-empty → use it.
        float2 nuv = float2(i.uv.x, u);
        n = NoiseTex.SampleLevel(g_LinearWrap, nuv, 0).r;
    }
    else
    {
        n = ProceduralNoise1D(u, 0.0);
    }
    n = lerp(saturate(NoiseFloor), 1.0, n);

    // ---- Compose emissive --------------------------------------------------
    float3 baseCol = BeamColor.rgb * Intensity * n;

    // Hot-white centre band — lerp toward HotCoreColor where rim is low
    // (centre of the beam silhouette). Boosts the "white-hot" reading.
    baseCol = lerp(HotCoreColor.rgb, baseCol, saturate(side));

    // Add fresnel rim emissive on top.
    baseCol += BeamColor.rgb * RimIntensity * rim;

    // ---- TAA velocity ------------------------------------------------------
    float2 curNDC  = i.curClip.xy  / i.curClip.w;
    float2 prevNDC = i.prevClip.xy / i.prevClip.w;

    // ---- Output ------------------------------------------------------------
    const bool excludeSSAO = (mat.materialFlags & MAT_FLAG_EXCLUDE_FROM_SSAO) != 0;
    const float matIdxEnc  = excludeSSAO ? -(float(materialIndex) + 1.0)
                                         :  float(materialIndex);

    GOut o;
    o.albedo   = float4(baseCol, 1.0);
    o.normal   = float4(normalize(i.wn) * 0.5 + 0.5, matIdxEnc);
    o.surface  = float4(saturate(Roughness), 0.0, 1.0, 0.04);
    o.velocity = curNDC - prevNDC;
    o.extra    = float4(0, 0, 0, 0);
    // Beam is Unlit — its glow comes through albedo via Lighting.ps's Unlit
    // variant (which additive-blends albedo onto sceneCol). No separate
    // emissive seed needed; sceneCol stays 0.
    o.sceneCol = float4(0, 0, 0, 1);
    return o;
}
