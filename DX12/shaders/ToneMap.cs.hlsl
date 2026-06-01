// ToneMap.cs.hlsl
// Reads HDR scene + bloom texture + exposure, writes LDR output.
// ACES fitted tone mapping (Krzysztof Narkowicz approximation).
//
// Root signature (space2):
//   [0] cbuffer PerDispatch b0 space2
//   [1] SRV t0 space2  — HDR scene
//   [2] SRV t1 space2  — bloom (mip 0, composited from BloomPass)
//   [3] SRV t2 space2  — exposure buffer (StructuredBuffer<float>)
//   [7] SRV t3 space2  — lens flare (half-res RGBA16F additive)
//   [4] UAV u0 space2  — LDR output (RGBA8 UNORM)

cbuffer PerDispatch : register(b0, space2)
{
    uint  g_width;
    uint  g_height;
    float g_bloomStrength;   // additive bloom intensity (0..1)
    uint  g_enableLUT;       // 1 = apply color grading LUT, 0 = bypass
    float g_lensFlareStrength; // additive lens flare multiplier (0 = disabled)
    float _tmPad0;
    float _tmPad1;
    float _tmPad2;
}

Texture2D<float4>        g_hdr       : register(t0, space2);
Texture2D<float4>        g_bloom     : register(t1, space2);
StructuredBuffer<float>  g_exposure  : register(t2, space2);
Texture2D<float4>        g_lensFlare : register(t3, space2);
Texture3D<float4>        g_lut       : register(t5, space2);
RWTexture2D<float4>      g_output    : register(u0, space2);

SamplerState g_linear : register(s0, space2);

static const float LUT_SIZE   = 32.0;
static const float LUT_SCALE  = (LUT_SIZE - 1.0) / LUT_SIZE;
static const float LUT_OFFSET = 0.5 / LUT_SIZE;

// ACES Fitted (Stephen Hill / "s2014") — full hue-preserving RRT+ODT fit
// in ACEScg (AP1) space. This is the tonemap UE's "ACES" output transform uses.
// Highlights desaturate naturally (saturated blues from Rayleigh scattering
// roll off toward white instead of staying pure blue).
//
// Pipeline:
//   sRGB linear → ACEScg (AP1)  via ACESInputMat
//   tonemap S-curve in AP1      via RRTAndODTFit
//   ACEScg → sRGB linear        via ACESOutputMat
//
// Matrices fold (sRGB→AP0→AP1) and (AP1→sRGB) into single 3×3s.
static const float3x3 ACESInputMat = {
    { 0.59719f, 0.35458f, 0.04823f },
    { 0.07600f, 0.90834f, 0.01566f },
    { 0.02840f, 0.13383f, 0.83777f }
};
static const float3x3 ACESOutputMat = {
    {  1.60475f, -0.53108f, -0.07367f },
    { -0.10208f,  1.10813f, -0.00605f },
    { -0.00327f, -0.07276f,  1.07602f }
};

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 ACESFitted(float3 color)
{
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(g_width, g_height);

    float3 hdr   = g_hdr.SampleLevel(g_linear, uv, 0).rgb;
    float3 bloom = g_bloom.SampleLevel(g_linear, uv, 0).rgb;
    float3 flare = g_lensFlare.SampleLevel(g_linear, uv, 0).rgb;

    float exposure = g_exposure[0];

    // Combine HDR + bloom + lens flare
    float3 color = hdr + bloom * g_bloomStrength + flare * g_lensFlareStrength;

    // Apply exposure
    color *= exposure;

    // ACES tone mapping (Stephen Hill fitted RRT+ODT, hue-preserving)
    color = ACESFitted(color);

    // Linear → sRGB gamma (approximate: pow(c, 1/2.2))
    color = pow(max(color, 0.0f), 1.0f / 2.2f);

    // Apply color grading LUT (after tonemap + gamma, in sRGB [0,1])
    if (g_enableLUT)
    {
        float3 uvw = saturate(color) * LUT_SCALE + LUT_OFFSET;
        color = g_lut.SampleLevel(g_linear, uvw, 0).rgb;
    }

    g_output[id.xy] = float4(color, 1.0f);
}
