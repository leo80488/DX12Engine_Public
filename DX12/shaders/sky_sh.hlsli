#ifndef SKY_SH_HLSLI
#define SKY_SH_HLSLI

// sky_sh.hlsli — SkyIBLPass-projected L2 spherical-harmonic diffuse.
//
// SkyIBLPass projects whichever cubemap is currently driving the sky
// (procedural atmosphere or a static .itex) into 9 × float4 SH coefficients
// each frame (atmosphere mode) or once (static mode). SkySHProjection.cs
// stores raw radiance projection L_lm = ∫ L(ω) Y_lm(ω) dω. EvalSH2(N)
// convolves with the clamped-cosine kernel (Ramamoorthi & Hanrahan 2001)
// so the returned value is diffuse irradiance / π — directly usable as
// `albedo * EvalSH2(N)` in a Lambertian shader.
//
// Convolution constants:
//   A_0 = π,  A_1 = 2π/3,  A_2 = π/4
// Folded with 1/π so the consumer can do `albedo * EvalSH2(N)` directly:
//   c0 = 1.0,  c1 = 2/3,  c2 = 1/4
//
// Why this matters for Rayleigh atmospheres: the radiance dome is strongly
// blue at zenith and warmer at horizon. Raw radiance reconstruction (no
// convolution) returns the band-limited L(N), so an upward-facing normal
// gets hit with the peak blue value and a white object visibly tints blue.
// The clamped-cosine convolution suppresses the L=2 band by 4× — which is
// exactly the band that carries the zenith-vs-horizon non-uniformity. The
// output is the correct physical irradiance/π. Unreal/Unity-equivalent.
//
// Magnitude conventions (both paths share this convention):
//   sky_sh.hlsli   : returns E(N)/π  (this file)
//   DDGI           : returns E(N)/π  (DDGICommon.hlsli::DDGI_SH_Irradiance)
//   gIrradiance    : returns E(N)/π  (standard prefiltered irradiance cube)
//
// Used by both the deferred LightingPass and forward TransparentPass — the
// same buffer, the same evaluator. Whether to call it at all is gated on
// LightCB.iblUseSH so static-skybox scenes still sample gIrradiance.
//
// Slot convention (must match LightingPass.cpp / TransparentPass.cpp root sig):
//   t19 space0 — gSkySH StructuredBuffer<float4>
//
// SH basis matches SkySHProjection.cs.hlsl — keep the constants synced.

StructuredBuffer<float4> gSkySH : register(t19, space0);

float3 EvalSH2(float3 N)
{
    // A_l / π — Ramamoorthi & Hanrahan clamped-cosine convolution / π.
    const float c0 = 1.0;        // A_0/π = π/π
    const float c1 = 2.0 / 3.0;  // A_1/π = (2π/3)/π
    const float c2 = 1.0 / 4.0;  // A_2/π = (π/4)/π

    float b0 = 0.282095;
    float b1 = 0.488603 * N.y;
    float b2 = 0.488603 * N.z;
    float b3 = 0.488603 * N.x;
    float b4 = 1.092548 * N.x * N.y;
    float b5 = 1.092548 * N.y * N.z;
    float b6 = 0.315392 * (3.0 * N.z * N.z - 1.0);
    float b7 = 1.092548 * N.z * N.x;
    float b8 = 0.546274 * (N.x * N.x - N.y * N.y);

    return max(0.0,
        gSkySH[0].rgb * (b0 * c0) +
        gSkySH[1].rgb * (b1 * c1) +
        gSkySH[2].rgb * (b2 * c1) +
        gSkySH[3].rgb * (b3 * c1) +
        gSkySH[4].rgb * (b4 * c2) +
        gSkySH[5].rgb * (b5 * c2) +
        gSkySH[6].rgb * (b6 * c2) +
        gSkySH[7].rgb * (b7 * c2) +
        gSkySH[8].rgb * (b8 * c2));
}

#endif // SKY_SH_HLSLI
