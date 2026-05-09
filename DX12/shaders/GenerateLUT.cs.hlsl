// GenerateLUT.cs.hlsl
// Bakes a 32x32x32 procedural 3D color grading LUT.
// Each texel represents an RGB input → graded RGB output.
//
// Root signature (shared compute, space2):
//   [0] CBV  b0 space2  — ColorGradingParams
//   [5] UAV  u1 space2  — RWTexture3D output LUT

cbuffer ColorGradingCB : register(b0, space2)
{
    // Exposure & Contrast
    float cb_exposure;
    float cb_contrast;
    float cb_brightness;
    float _pad0;

    // Lift / Gamma / Gain
    float3 cb_lift;   float _pad1;
    float3 cb_gamma;  float _pad2;
    float3 cb_gain;   float _pad3;

    // HSL
    float cb_hueShift;
    float cb_saturation;
    float cb_vibrance;
    float _pad4;

    // White Balance
    float cb_temperature;
    float cb_tint;
    float _pad5a, _pad5b;

    // Vignette & Grain (unused in LUT bake, but present for layout match)
    float cb_vignetteStrength;
    float cb_filmGrain;
    float _pad6a, _pad6b;
};

RWTexture3D<float4> g_lutOut : register(u1, space2);

static const uint LUT_SIZE = 32;

// ---- Utility ---------------------------------------------------------------

float3 ApplyWhiteBalance(float3 c, float temp, float tint)
{
    float t = temp * 0.1;
    float3 wb = float3(1.0 + t, 1.0 + tint * 0.05, 1.0 - t);
    return c * wb;
}

float3 ApplyExposure(float3 c, float ev)
{
    return c * pow(2.0, ev);
}

float3 ApplyLiftGammaGain(float3 c, float3 lift, float3 gamma, float3 gain)
{
    c = c * gain + lift;
    c = pow(max(c, 0.001), 1.0 / max(gamma, 0.001));
    return c;
}

float3 ApplyContrast(float3 c, float contrast)
{
    return saturate((c - 0.5) * contrast + 0.5);
}

float3 RGBtoHSV(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 HSVtoRGB(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

float3 ApplyHueSatVibrance(float3 c, float hueShift, float sat, float vib)
{
    float3 hsv = RGBtoHSV(c);
    hsv.x = frac(hsv.x + hueShift / 360.0);
    float vibranceAdj = (1.0 - hsv.y) * vib;
    hsv.y = saturate(hsv.y * sat + vibranceAdj);
    return HSVtoRGB(hsv);
}

// ---- Main ------------------------------------------------------------------

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= LUT_SIZE)) return;

    // Neutral color: map texel coordinate to [0,1] RGB
    float3 color = (float3(id) + 0.5) / float(LUT_SIZE);

    // 1. White Balance
    color = ApplyWhiteBalance(color, cb_temperature, cb_tint);

    // 2. Exposure
    color = ApplyExposure(color, cb_exposure);

    // 3. Lift / Gamma / Gain
    color = ApplyLiftGammaGain(color, cb_lift, cb_gamma, cb_gain);

    // 4. Contrast
    color = ApplyContrast(color, cb_contrast);

    // 5. Brightness
    color = saturate(color + cb_brightness);

    // 6. Hue / Saturation / Vibrance
    color = ApplyHueSatVibrance(color, cb_hueShift, cb_saturation, cb_vibrance);

    g_lutOut[id] = float4(saturate(color), 1.0);
}
