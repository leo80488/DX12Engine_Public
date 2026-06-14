// HeightFogApply.ps.hlsl — UE-style analytic exponential height fog.
// -----------------------------------------------------------------------------
// Fullscreen triangle drawn AFTER CloudPass (HDR already holds lit geometry +
// aerial perspective + skybox + water + clouds) and BEFORE VideoPass / the
// froxel VolumetricFogPass (near volumetric detail composes OVER this far
// fog). Blend ONE / INV_SRC_ALPHA: out = inscatter·opacity + scene·(1−opacity).
//
// Density along the view ray p(t) = O + D·t (world metres, Y-up):
//   σ(t) = d0 · exp(−k · (O.y + D.y·t − H))
// has the closed-form optical depth over [t0, t1] (len = t1 − t0):
//   τ = d0·exp(−k·(O.y + D.y·t0 − H)) · (1 − exp(−k·D.y·len)) / (k·D.y)
// with the D.y→0 limit handled by a Taylor branch. Sky pixels use the
// analytic t→∞ limit: τ∞ = startDensity / (k·D.y) for climbing rays,
// divergent (→ MaxOpacity) for level/descending rays — no arbitrary "sky
// distance" tunable, and no seam against far geometry.
//
// Graphics root signature only: LightCB at b1 (sun dir/color, cameraPos,
// invViewProj), HeightFogCB at b2, depth at t5 space0.
// -----------------------------------------------------------------------------

#define LIGHT_CB_REGISTER b1
#include "light_cb.hlsli"      // lightDir (FROM-light), lightColor, cameraPos, invViewProj
#include "DepthCommon.hlsli"   // WorldPosFromDepth, ViewRayFromUV
#include "FroxelCommon.hlsli"  // PhaseHG (no register bindings in this header)

cbuffer HeightFogCB : register(b2, space0)
{
    float  FogDensity;          // d0: extinction at h == FogHeight (1/m)
    float  FogHeightFalloff;    // k (1/m)
    float  FogHeight;           // H, world metres
    float  StartDistance;       // metres of fog-free range in front of the camera

    float3 FogColor;            // ambient inscatter (linear HDR)
    float  MaxOpacity;          // UE FogMaxOpacity (0..1)

    float  SunInscatterIntensity; // 0 disables the sun term
    float  Anisotropy;            // HG g toward the sun
    float  _hfPad0;
    float  _hfPad1;
};

Texture2D<float> gDepth  : register(t5, space0);
SamplerState     gLinear : register(s0);

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSIn i) : SV_TARGET
{
    if (FogDensity <= 0.0) return float4(0, 0, 0, 0);

    // Load (not bilinear) — interpolated depth across a geometry↔sky edge
    // produces phantom mid-range distances and a 1-px fog halo at silhouettes.
    float ndcZ = gDepth.Load(int3(int2(i.pos.xy), 0));

    float3 rayDir;
    float  tau;

    if (ndcZ <= 0.0001)   // sky (reversed-Z: far = 0) — fog the backdrop too
    {
        rayDir = ViewRayFromUV(i.uv, invViewProj);

        float relY = cameraPos.y + rayDir.y * StartDistance - FogHeight;
        float startDensity = FogDensity
                           * exp(clamp(-FogHeightFalloff * relY, -80.0, 80.0));
        float kdy = FogHeightFalloff * rayDir.y;
        // t→∞: converges only for rays climbing out of the layer; level or
        // descending rays accumulate unbounded fog → saturate to MaxOpacity.
        tau = (kdy > 1e-6) ? startDensity / kdy : 1e9;
    }
    else
    {
        float3 wp = WorldPosFromDepth(i.uv, ndcZ, invViewProj);
        float3 d  = wp - cameraPos;
        float dist = max(length(d), 1e-4);
        rayDir = d / dist;

        float t0  = min(StartDistance, dist);
        float len = dist - t0;
        if (len <= 0.0) return float4(0, 0, 0, 0);

        // exp args clamped to ±80 — looking down from far above the layer
        // makes −k·relY large; unclamped exp() → inf → NaN after the divide.
        float relY = cameraPos.y + rayDir.y * t0 - FogHeight;
        float startDensity = FogDensity
                           * exp(clamp(-FogHeightFalloff * relY, -80.0, 80.0));
        float kdy = FogHeightFalloff * rayDir.y;
        float u   = kdy * len;

        tau = (abs(u) > 1e-3)
            ? startDensity * (1.0 - exp(clamp(-u, -80.0, 80.0))) / kdy
            : startDensity * len * (1.0 - 0.5 * u);   // D.y ≈ 0 Taylor limit
    }

    tau = max(tau, 0.0);
    float T       = exp(-min(tau, 80.0));
    float opacity = min(1.0 - T, MaxOpacity);

    float3 dirToSun  = -lightDir;   // LightCB lightDir is FROM-light
    float3 inscatter = FogColor
                     + lightColor * SunInscatterIntensity
                       * PhaseHG(dot(rayDir, dirToSun), Anisotropy);

    return float4(inscatter * opacity, opacity);
}
