// AerialPerspective.cs.hlsl — Hillaire 2020 aerial perspective 3D LUT.
// -----------------------------------------------------------------------------
// Output: 32×32×32 R16G16B16A16_FLOAT.
//   xy → screen UV (0..1)
//   z  → slice index in [0, kApDepthSlices), linearly mapped to [0, kMaxDistKm]
//   value.rgb = in-scatter radiance accumulated from camera up to slice depth
//   value.a   = transmittance (scalar) from camera to slice depth
//
// Per-pixel composite (see Lighting.ps.hlsl):
//   slice = viewDistKm / kMaxDistKm
//   ap    = AerialPerspectiveLUT.Sample(uv, slice)
//   color = sceneColor * ap.a + ap.rgb * SunIntensity
//
// Dispatch: (32/8, 32/8, 32) = (4, 4, 32), threadgroup (8, 8, 1) — one thread
// per voxel (x, y) with z = DTid.z.
//
// Root sig:
//   [0] CBV  b0 space2 — AerialCB
//   [1] SRV  t0 space2 — TransmittanceLUT
//   [2] SRV  t1 space2 — MultiScatterLUT
//   [4] UAV  u0 space2 — output (RWTexture3D<float4>)
// -----------------------------------------------------------------------------

#include "AtmosphereCommon.hlsli"

cbuffer AerialCB : register(b0, space2)
{
    float3 SunDir;               float CameraAltitudeKm;
    float3 SunColor;             float MaxDistKm;      // far slice range
    float4x4 InvViewProj;        // world-from-clip (unjittered)
    float3 CameraWorldPos;       float _pad0;
    float3 CameraForwardWorld;   float _pad1;
    uint   LutW;
    uint   LutH;
    uint   LutD;
    uint   _pad2;
};

Texture2D<float4>    TransmittanceLUT : register(t0, space2);
Texture2D<float4>    MultiScatterLUT  : register(t1, space2);
RWTexture3D<float4>  DstLUT           : register(u0, space2);
SamplerState         LinClamp         : register(s0, space2);

static const int kApStepsPerSlice = 6;

float3 SampleTrans(float h, float mu)
{
    float2 uv; ParamsToLutUv(h, mu, uv);
    return TransmittanceLUT.SampleLevel(LinClamp, uv, 0).rgb;
}
float3 SampleMs(float h, float mu)
{
    float2 uv; ParamsToLutUv(h, mu, uv);
    return MultiScatterLUT.SampleLevel(LinClamp, uv, 0).rgb;
}

// Reconstruct world-space direction for voxel (u, v).
// Shoots a ray from the camera through screen(uv). Reversed-Z convention:
// near plane lives at NDC z=1, far plane at NDC z=0 — so pNear uses z=1.
float3 ReconstructViewDir(float2 uv)
{
    // NDC from uv: y flipped (D3D screen-top=0, NDC-top=+1)
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 pNear = mul(float4(ndc, 1.0, 1.0), InvViewProj);
    float4 pFar  = mul(float4(ndc, 0.0, 1.0), InvViewProj);
    pNear /= pNear.w;
    pFar  /= pFar.w;
    return normalize(pFar.xyz - pNear.xyz);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= LutW || DTid.y >= LutH || DTid.z >= LutD) return;

    // Voxel (u, v) → world view ray.
    float2 uv   = (float2(DTid.xy) + 0.5) / float2(LutW, LutH);
    float3 rd   = ReconstructViewDir(uv);

    // Slice Z maps to a distance along the view ray. Quadratic distribution
    // packs more resolution up close (Hillaire eq.).
    float sliceT = (float(DTid.z) + 0.5) / float(LutD);
    float distKm = MaxDistKm * sliceT * sliceT;

    // Origin at camera altitude, "up" = world Y (simplified — geometry uses
    // ground-plane approximation, matching SkyView LUT).
    float3 origin = float3(0.0, kGroundR + CameraAltitudeKm, 0.0);

    // Raymarch from camera along rd for `distKm`.
    int   steps = kApStepsPerSlice;
    float dt    = distKm / float(steps);
    float3 sunDirN = normalize(SunDir);
    float  cosT    = dot(rd, sunDirN);
    float  phaseR  = RayleighPhase(cosT);
    float  phaseM  = MiePhaseHG(cosT, kMiePhaseG);

    float3 L       = 0;
    float3 T       = float3(1, 1, 1);

    [loop] for (int i = 0; i < steps; ++i)
    {
        float  t    = dt * (float(i) + 0.5);
        float3 p    = origin + rd * t;
        float  r    = length(p);
        float  alt  = r - kGroundR;
        AtmoMedium m = SampleAtmosphere(alt);

        float3 stepT = exp(-m.extinction * dt);

        float  muSun = dot(p / max(r, 1e-6), sunDirN);
        float3 Tsun  = SampleTrans(alt, muSun);
        float  tBlock = RaySphere(p, sunDirN, kGroundR);
        if (tBlock > 0.0) Tsun = 0;

        float3 S_single = (m.sctRayleigh * phaseR
                        + float3(m.sctMie, m.sctMie, m.sctMie) * phaseM) * Tsun;
        float3 Fms = SampleMs(alt, muSun);
        float3 S   = S_single + m.scattering * Fms;

        float3 Sint = (S - S * stepT) / max(m.extinction, float3(1e-6, 1e-6, 1e-6));
        L += T * Sint;
        T *= stepT;
    }

    // Transmittance alpha = average of channels (scalar approximation).
    float tAvg = (T.r + T.g + T.b) / 3.0;

    // Pre-multiply in-scatter by SunColor so the consumer just adds.
    DstLUT[DTid] = float4(L * SunColor, tAvg);
}
