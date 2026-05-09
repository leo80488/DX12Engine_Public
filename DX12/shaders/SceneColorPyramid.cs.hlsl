// SceneColorPyramid.cs.hlsl — pre-filtered HDR mip pyramid for SSR cone fetch.
//
// Built per frame from the race-free HDR snapshot the resolve pass owns.
// Resolve samples this at a mip level derived from roughness × ray length
// so glossy reflections get a pre-integrated radiance value instead of a
// single mip-0 texel — the core variance reduction for rough SSR noise.
//
// Reduce uses Karis firefly weighting (w = 1/(1+lum)) so blown-out texels
// don't survive the 2×2 average as specular sparkles.

cbuffer HierCB : register(b0, space2)
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
};

Texture2D<float4>    g_Src : register(t0, space2);   // mip 0 path: HDR snapshot
RWTexture2D<float4>  g_Mip : register(u0, space2);   // mip 0 dst / reduce src
RWTexture2D<float4>  g_Dst : register(u1, space2);   // reduce dst

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// CSMain_Mip0 entry kept as a no-op so ShaderID::SceneColorPyramidMip0_CS
// still resolves at Init time; the actual mip 0 generation now uses
// CopyTextureSubresource in SceneColorPyramidPass::Execute (the CS write
// path silently dropped writes to this pyramid for reasons we couldn't
// isolate — see the comment in that method).
[numthreads(8, 8, 1)]
void CSMain_Mip0(uint3 dtid : SV_DispatchThreadID)
{
    // Intentionally empty.
}

[numthreads(8, 8, 1)]
void CSMain_Reduce(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dstWidth || dtid.y >= dstHeight) return;

    uint2 s0 = uint2(min(dtid.x * 2u,      srcWidth - 1u), min(dtid.y * 2u,      srcHeight - 1u));
    uint2 s1 = uint2(min(dtid.x * 2u + 1u, srcWidth - 1u), min(dtid.y * 2u,      srcHeight - 1u));
    uint2 s2 = uint2(min(dtid.x * 2u,      srcWidth - 1u), min(dtid.y * 2u + 1u, srcHeight - 1u));
    uint2 s3 = uint2(min(dtid.x * 2u + 1u, srcWidth - 1u), min(dtid.y * 2u + 1u, srcHeight - 1u));

    float4 c0 = g_Mip[s0];
    float4 c1 = g_Mip[s1];
    float4 c2 = g_Mip[s2];
    float4 c3 = g_Mip[s3];

    float w0 = 1.0 / (1.0 + Luminance(c0.rgb));
    float w1 = 1.0 / (1.0 + Luminance(c1.rgb));
    float w2 = 1.0 / (1.0 + Luminance(c2.rgb));
    float w3 = 1.0 / (1.0 + Luminance(c3.rgb));
    float wSum = w0 + w1 + w2 + w3;

    g_Dst[dtid.xy] = (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / wSum;
}
