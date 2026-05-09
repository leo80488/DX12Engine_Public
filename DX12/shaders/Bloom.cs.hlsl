// Bloom.cs.hlsl
// Bloom downsample (BLOOM_DOWNSAMPLE=1) and upsample (BLOOM_DOWNSAMPLE=0).
//
// Downsample: 13-tap Sledgehammer kernel with Karis Average on the first pass.
// Upsample:   3x3 tent filter, additively blended.
//
// Compute root signature (space2):
//   [0] cbuffer PerDispatch b0 space2
//   [1] SRV t0 space2  — input texture
//   [4] UAV u0 space2  — output texture

cbuffer PerDispatch : register(b0, space2)
{
    uint  g_srcWidth;
    uint  g_srcHeight;
    uint  g_dstWidth;
    uint  g_dstHeight;
    float g_filterRadius;   // upsample tent radius (pixels in dst space)
    uint  g_firstDownsample; // 1 = apply Karis Average
    float _pad0;
    float _pad1;
}

Texture2D<float4>   g_input  : register(t0, space2);
RWTexture2D<float4> g_output : register(u0, space2);

SamplerState g_linear : register(s0, space2);

// ------------------------------------------------------------------
// Luminance helper (BT.709)
// ------------------------------------------------------------------
float Luma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Karis Average: luminance-weighted average that suppresses fireflies.
float4 KarisAverage(float4 a, float4 b, float4 c, float4 d)
{
    float wa = 1.0f / (1.0f + Luma(a.rgb));
    float wb = 1.0f / (1.0f + Luma(b.rgb));
    float wc = 1.0f / (1.0f + Luma(c.rgb));
    float wd = 1.0f / (1.0f + Luma(d.rgb));
    float sum = wa + wb + wc + wd;
    return (a * wa + b * wb + c * wc + d * wd) / sum;
}

// ------------------------------------------------------------------
// 13-tap Sledgehammer downsample
// Samples 5 overlapping 2x2 quads around the center texel.
// ------------------------------------------------------------------
float4 DownsampleBox13(float2 uv)
{
    float2 texelSize = float2(1.0f / float(g_srcWidth), 1.0f / float(g_srcHeight));
    float2 e = texelSize;

    // 5 groups of 2x2 quad samples
    // Center: weight 0.5   Corners: weight 0.125 each  → total = 1.0
    float4 a = g_input.SampleLevel(g_linear, uv + float2(-2.0f,  2.0f) * e, 0);
    float4 b = g_input.SampleLevel(g_linear, uv + float2( 0.0f,  2.0f) * e, 0);
    float4 c = g_input.SampleLevel(g_linear, uv + float2( 2.0f,  2.0f) * e, 0);

    float4 d = g_input.SampleLevel(g_linear, uv + float2(-2.0f,  0.0f) * e, 0);
    float4 e_center = g_input.SampleLevel(g_linear, uv + float2( 0.0f,  0.0f) * e, 0);
    float4 f = g_input.SampleLevel(g_linear, uv + float2( 2.0f,  0.0f) * e, 0);

    float4 g = g_input.SampleLevel(g_linear, uv + float2(-2.0f, -2.0f) * e, 0);
    float4 h = g_input.SampleLevel(g_linear, uv + float2( 0.0f, -2.0f) * e, 0);
    float4 i = g_input.SampleLevel(g_linear, uv + float2( 2.0f, -2.0f) * e, 0);

    float4 j = g_input.SampleLevel(g_linear, uv + float2(-1.0f,  1.0f) * e, 0);
    float4 k = g_input.SampleLevel(g_linear, uv + float2( 1.0f,  1.0f) * e, 0);
    float4 l = g_input.SampleLevel(g_linear, uv + float2(-1.0f, -1.0f) * e, 0);
    float4 m = g_input.SampleLevel(g_linear, uv + float2( 1.0f, -1.0f) * e, 0);

    float4 downsample = e_center * 0.125f;
    downsample += (a + c + g + i) * 0.03125f;
    downsample += (b + d + f + h) * 0.0625f;
    downsample += (j + k + l + m) * 0.125f;

    float4 k_grp0 = KarisAverage(j, k, l, m) * 0.5f; 
    float4 k_grp1 = KarisAverage(a, b, d, e_center) * 0.125f;
    float4 k_grp2 = KarisAverage(b, c, e_center, f) * 0.125f;
    float4 k_grp3 = KarisAverage(d, e_center, g, h) * 0.125f;
    float4 k_grp4 = KarisAverage(e_center, f, h, i) * 0.125f;
    
    if (g_firstDownsample)
    {
        downsample = k_grp0 + k_grp1 + k_grp2 + k_grp3 + k_grp4;
    }

    return max(downsample, 0.0001f);
}

// ------------------------------------------------------------------
// 3x3 tent upsample
// ------------------------------------------------------------------
float4 UpsampleTent(float2 uv)
{
    float2 texelSize = float2(1.0f / float(g_srcWidth), 1.0f / float(g_srcHeight));
    float r = g_filterRadius;

    float4 sum = float4(0, 0, 0, 0);
    // Weights: corners=1, edges=2, center=4  (total=16)
    sum += g_input.SampleLevel(g_linear, uv + float2(-r, -r) * texelSize, 0) * 1.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( 0, -r) * texelSize, 0) * 2.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( r, -r) * texelSize, 0) * 1.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2(-r,  0) * texelSize, 0) * 2.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( 0,  0) * texelSize, 0) * 4.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( r,  0) * texelSize, 0) * 2.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2(-r,  r) * texelSize, 0) * 1.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( 0,  r) * texelSize, 0) * 2.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( r,  r) * texelSize, 0) * 1.0f;
    return sum / 16.0f;
}

// ------------------------------------------------------------------
// Entry points
// ------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstWidth || id.y >= g_dstHeight) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(g_dstWidth, g_dstHeight);
    g_output[id.xy] = DownsampleBox13(uv);
}

[numthreads(8, 8, 1)]
void CSUpsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstWidth || id.y >= g_dstHeight) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(g_dstWidth, g_dstHeight);
    float4 upsampled = UpsampleTent(uv);

    // Additive blend into the existing lower-mip content
    g_output[id.xy] = g_output[id.xy] + upsampled;
}
