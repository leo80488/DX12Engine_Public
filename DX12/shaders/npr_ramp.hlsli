#ifndef NPR_RAMP_HLSLI
#define NPR_RAMP_HLSLI

// npr_ramp.hlsli — NPR skin ramp texture helpers.
//
// Ramp texture: 25 rows, each row height = 1/25.
// Row center V = (row + 0.5) / 25.
//
// Key rows:
//   Row  2 (V= 2.5/25): Base skin color (warm half-lambert)
//   Row  7 (V= 7.5/25): Mid-tone (yellow-ish transition in shadow side)
//   Row 11 (V=11.5/25): SSS (thin skin: ears, nostrils, lips)
//   Row 17 (V=17.5/25): Deep shadow / subsurface vein (cool undertone)
//   Row 22 (V=22.5/25): Blood color / rim
//
// Requires (declared before including):
//   - Texture2D<float4> gRampTex
//   - SamplerState gSampler (linear-clamp)

static const float NPR_ROW_BASE_SKIN   =  2.5 / 25.0;  // row 2
static const float NPR_ROW_MID_SKIN    =  7.5 / 25.0;  // row 7
static const float NPR_ROW_SSS         = 11.5 / 25.0;  // row 11
static const float NPR_ROW_SHADOW_VEIN = 17.5 / 25.0;  // row 17
static const float NPR_ROW_BLOOD_RIM   = 22.5 / 25.0;  // row 22

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Sample a specific row of the ramp texture.
// U = half-lambert * AO modulation, V = row center.
float3 SampleRamp(float u, float rowV)
{
    return gRampTex.SampleLevel(gSampler, float2(saturate(u), rowV), 0).rgb;
}

// Compute ramp U coordinate from NdotL + AO.
// aoInfluence controls how much AO darkens the ramp lookup (0.8~1.0 typical).
float RampU(float rawNdotL, float ao, float aoInfluence)
{
    float halfLambert = rawNdotL * 0.5 + 0.5;
    return saturate(halfLambert * lerp(1.0, ao, aoInfluence));
}

// Full skin NPR diffuse shading.
// Returns lit diffuse color (already tinted by ramp, multiply with albedo externally).
//   rawNdotL:    unclamped dot(N, L)
//   ao:          ambient occlusion [0,1]
//   aoInfluence: how much AO affects ramp U (0.8~1.0)
//   shadowFactor: CSM shadow [0,1]
float3 NPRSkinDiffuse(float rawNdotL, float ao, float aoInfluence, float shadowFactor)
{
    float u = RampU(rawNdotL, ao, aoInfluence) * shadowFactor;

    // 1. Base skin (warm half-lambert)
    float3 baseRamp = SampleRamp(u, NPR_ROW_BASE_SKIN);

    // 2. Mid-tone (yellow transition in shadow-lit boundary)
    float3 midRamp  = SampleRamp(u, NPR_ROW_MID_SKIN);

    // 3. Deep shadow vein (cool subsurface in dark areas)
    float  halfLambert = rawNdotL * 0.5 + 0.5;
    float  shadowMask  = saturate(1.0 - halfLambert * 2.0); // 1 in deep shadow, 0 in lit
    float3 veinRamp    = SampleRamp(u, NPR_ROW_SHADOW_VEIN);

    // Blend layers
    float3 diffuse = baseRamp;
    diffuse = lerp(diffuse, midRamp,  0.4);                  // mid-tone blend
    diffuse = lerp(diffuse, veinRamp, shadowMask * ao * 0.5); // dark vein in shadow

    return diffuse;
}

// SSS color for thin skin areas (ears, nostrils, lips).
// Returns ramp color to be additively blended with sss mask weight.
float3 NPRSkinSSS(float rawNdotL, float ao, float aoInfluence, float shadowFactor)
{
    float u = RampU(rawNdotL, ao, aoInfluence) * shadowFactor;
    return SampleRamp(u, NPR_ROW_SSS);
}

// Rim / blood color for NPR rim light.
float3 NPRRimColor(float NdotV)
{
    return SampleRamp(1.0 - NdotV, NPR_ROW_BLOOD_RIM);
}

#endif // NPR_RAMP_HLSLI
