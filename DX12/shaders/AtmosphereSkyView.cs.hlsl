// AtmosphereSkyView.cs.hlsl
// -----------------------------------------------------------------------------
// SkyViewLUT (192×108, R16G16B16A16_FLOAT). Runs EVERY FRAME.
//
// For a fixed camera altitude, encodes the sky radiance along each view
// direction parameterised by (azimuth φ ∈ [0, 2π], elevation θ ∈ [-π/2, π/2]).
// Uses the non-linear V parameterisation for extra horizon precision.
//
// Samples TransmittanceLUT + MultiScatterLUT to compose per-step scattering.
//
// Root sig:
//   [0] CBV  b0 space2 — SkyViewCB
//   [1] SRV  t0 space2 — TransmittanceLUT
//   [2] SRV  t1 space2 — MultiScatterLUT
//   [4] UAV  u0 space2 — output
// -----------------------------------------------------------------------------

#include "AtmosphereCommon.hlsli"

cbuffer SkyViewCB : register(b0, space2)
{
    float3 sunDir;        float cameraAltitudeKm;
    uint   lutWidth;
    uint   lutHeight;
    uint   _pad0;
    uint   _pad1;
};

Texture2D<float4>   TransmittanceLUT : register(t0, space2);
Texture2D<float4>   MultiScatterLUT  : register(t1, space2);
RWTexture2D<float4> DstLUT           : register(u0, space2);
SamplerState        LinClamp         : register(s0, space2);

static const int kSvMaxSteps = 32;

float3 SampleTrans(float h, float mu)
{
    float2 uv; ParamsToLutUv(h, mu, uv);
    return TransmittanceLUT.SampleLevel(LinClamp, uv, 0).rgb;
}
float3 SampleMultiScatter(float h, float mu)
{
    float2 uv; ParamsToLutUv(h, mu, uv);
    return MultiScatterLUT.SampleLevel(LinClamp, uv, 0).rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= lutWidth || DTid.y >= lutHeight) return;

    float2 uv = (float2(DTid.xy) + 0.5) / float2(lutWidth, lutHeight);

    // LUT parameterisation:
    //   uv.x → azimuth in [0, 2π]
    //   uv.y → elevation via non-linear mapping (horizon has extra precision)
    float azimuth   = uv.x * 2.0 * ATMO_PI;
    float elevation = SkyViewVToElevation(uv.y);

    float cosEl = cos(elevation);
    float sinEl = sin(elevation);
    float3 rd   = float3(cosEl * cos(azimuth), sinEl, cosEl * sin(azimuth));

    float3 origin = float3(0.0, kGroundR + cameraAltitudeKm, 0.0);

    // Intersect atmosphere top / ground.
    float tTop    = RaySphere(origin, rd, kAtmoR);
    float tGround = RaySphere(origin, rd, kGroundR);
    float tMax    = (tGround > 0.0 && tGround < tTop) ? tGround : tTop;
    if (tMax < 0.0)
    {
        DstLUT[DTid.xy] = float4(0, 0, 0, 1);
        return;
    }

    float dt = tMax / float(kSvMaxSteps);
    float3 sunDirN = normalize(sunDir);
    float  cosT    = dot(rd, sunDirN);
    float  phaseR  = RayleighPhase(cosT);
    float  phaseM  = MiePhaseHG(cosT, kMiePhaseG);

    float3 L       = 0;
    float3 T       = float3(1, 1, 1);

    [loop] for (int i = 0; i < kSvMaxSteps; ++i)
    {
        float3 p   = origin + rd * (dt * (float(i) + 0.5));
        float  r   = length(p);
        float  alt = r - kGroundR;
        AtmoMedium m = SampleAtmosphere(alt);

        float3 stepT = exp(-m.extinction * dt);

        // Transmittance to sun from this point (look up the LUT).
        float muSun = dot(p / max(r, 1e-6), sunDirN);
        float3 Tsun = SampleTrans(alt, muSun);

        // Shadow by the planet: if the sample line to the sun intersects the
        // ground sphere, the sun light is blocked.
        float tBlock = RaySphere(p, sunDirN, kGroundR);
        if (tBlock > 0.0) Tsun = 0;

        // Directional (phase-weighted) single scatter.
        float3 S_single = (m.sctRayleigh * phaseR
                        + float3(m.sctMie, m.sctMie, m.sctMie) * phaseM) * Tsun;

        // Multi-scatter LUT contributes an isotropic term.
        float3 Fms = SampleMultiScatter(alt, muSun);
        float3 S_multi = m.scattering * Fms;

        float3 S = S_single + S_multi;
        float3 Sint = (S - S * stepT) / max(m.extinction, float3(1e-6, 1e-6, 1e-6));

        L += T * Sint;
        T *= stepT;
    }

    DstLUT[DTid.xy] = float4(L, 1.0);
}
