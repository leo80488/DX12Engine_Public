#ifndef SKY_SH_HLSLI
#define SKY_SH_HLSLI

// sky_sh.hlsli — SkyIBLPass-projected L2 spherical-harmonic diffuse.
//
// SkyIBLPass projects whichever cubemap is currently driving the sky
// (procedural atmosphere or a static .itex) into 9 × float4 SH coefficients
// each frame (atmosphere mode) or once (static mode). Consumers evaluate
// EvalSH2(N) at any direction to recover the band-limited diffuse value.
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
        gSkySH[0].rgb * b0 +
        gSkySH[1].rgb * b1 +
        gSkySH[2].rgb * b2 +
        gSkySH[3].rgb * b3 +
        gSkySH[4].rgb * b4 +
        gSkySH[5].rgb * b5 +
        gSkySH[6].rgb * b6 +
        gSkySH[7].rgb * b7 +
        gSkySH[8].rgb * b8);
}

#endif // SKY_SH_HLSLI
