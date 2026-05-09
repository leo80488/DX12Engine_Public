// SkyAtmosphere.cs.hlsl — Hillaire 2020 cubemap generator.
// -----------------------------------------------------------------------------
// Fills a 6-face cubemap by converting each face texel's direction to
// (azimuth, elevation) and sampling the SkyViewLUT written by
// AtmosphereSkyView.cs.hlsl. The sun disk is rendered analytically on top.
//
// Root sig:
//   [0] CBV  b0 space2 — AtmosphereCB
//   [1] SRV  t0 space2 — SkyViewLUT
//   [2] SRV  t1 space2 — TransmittanceLUT (for sun disk attenuation)
//   [4] UAV  u0 space2 — output RWTexture2DArray<float4>
// -----------------------------------------------------------------------------

#include "AtmosphereCommon.hlsli"

cbuffer AtmosphereCB : register(b0, space2)
{
    float3 SunDir;          float _pad0;
    float3 SunColor;        uint  FaceSize;
    float  CameraAltitudeKm;
    float  _pad1;
    float  _pad2;
    float  _pad3;
};

Texture2D<float4>         SkyViewLUT       : register(t0, space2);
Texture2D<float4>         TransmittanceLUT : register(t1, space2);
RWTexture2DArray<float4>  DstFace          : register(u0, space2);
SamplerState              LinClamp         : register(s0, space2);

float3 FaceUVToDir(uint face, float2 uv)
{
    float3 d;
    [branch] switch (face)
    {
        case 0: d = float3( 1.0, -uv.y, -uv.x); break;
        case 1: d = float3(-1.0, -uv.y,  uv.x); break;
        case 2: d = float3( uv.x,  1.0,  uv.y); break;
        case 3: d = float3( uv.x, -1.0, -uv.y); break;
        case 4: d = float3( uv.x, -uv.y,  1.0); break;
        default: d = float3(-uv.x, -uv.y, -1.0); break;
    }
    return normalize(d);
}

float3 SampleSky(float3 rd)
{
    float elevation = asin(clamp(rd.y, -1.0, 1.0));
    float azimuth   = atan2(rd.z, rd.x);
    if (azimuth < 0.0) azimuth += 2.0 * ATMO_PI;

    float2 uv = float2(azimuth / (2.0 * ATMO_PI), SkyViewElevationToV(elevation));
    return SkyViewLUT.SampleLevel(LinClamp, uv, 0).rgb;
}

// Sun disk is rendered directly in SkyboxPass (see Skybox.ps.hlsl) for
// pixel-resolution sharpness. Only scattering goes into this cubemap, which is
// what SH projection + specular pre-filter want anyway (a sharp sun would
// overwhelm both).
//
// Solar radiance multiplier — represents the sun's radiance OUTSIDE the
// atmosphere (top-of-atmosphere). The SkyView LUT already integrates
// transmittance from the sun to each sample point, so we must NOT pre-
// attenuate by the time-of-day sun colour here or we'd double-darken the
// horizon. `SunColor.rgb` from the CB is kept around only in case the user
// wants to tint the atmosphere further (e.g. coloured sun) — the magnitude
// we actually apply is the channel max so warmth doesn't crush blue.
static const float kSolarRadiance = 30.0;

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= FaceSize || DTid.y >= FaceSize) return;

    float2 uv  = (float2(DTid.xy) + 0.5) / float(FaceSize) * 2.0 - 1.0;
    float3 dir = FaceUVToDir(DTid.z, uv);

    float3 sky = SampleSky(dir);

    // User-driven brightness (comes from TickTimeOfDay via SunColor). We use
    // the max channel as a scale so the atmosphere LUT keeps producing its
    // own orange / blue gradients via internal transmittance. `kSolarRadiance`
    // bakes in the "top-of-atmosphere" luminance Hillaire assumes.
    float userScale = max(max(SunColor.r, SunColor.g), SunColor.b);
    // Reference scale: at noon our SunColor.max ≈ basePeak (5.0). Dividing by
    // a reference so the user's master Sun Intensity slider scales linearly
    // without over-dimming at sunset (when SunColor is tiny by design).
    userScale = max(userScale / 5.0, 0.05);

    float3 color = sky * kSolarRadiance * userScale;
    DstFace[uint3(DTid.xy, DTid.z)] = float4(color, 1.0);
}
