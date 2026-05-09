#ifndef TAA_HISTORY_HLSLI
#define TAA_HISTORY_HLSLI

// -----------------------------------------------------------------------------
// TAA_History.hlsli
//
// History reconstruction — full 9-tap Catmull-Rom bicubic.
//
// Why 9-tap (vs 5-tap 'optimised' variant): the 5-tap form drops the four
// corners by exploiting bilinear filter overlap on equal-weight quadrants.
// That optimisation is visually fine in steady state but loses energy on
// sharp diagonal edges and on subpixel-translated history during motion,
// which compounds with TAA's variance clip into edge softening over time.
// At TAA resolutions the 4 extra SampleLevels are not measurable.
//
// Reference: Falcor TAA.ps.slang::bicubicSampleCatmullRom
//   http://vec3.ca/bicubic-filtering-in-fewer-taps/
// -----------------------------------------------------------------------------

float3 SampleHistoryCatmullRom9Tap(Texture2D<float4> tex,
                                    SamplerState     samp,
                                    float2           samplePos, // pixel-space (uv * dim)
                                    float2           texDim)
{
    float2 invTex = 1.0 / texDim;

    float2 tc = floor(samplePos - 0.5) + 0.5;
    float2 f  = samplePos - tc;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = f2 - 0.5 * (f3 + f);
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w3 = 0.5 * (f3 - f2);
    float2 w2 = 1.0 - w0 - w1 - w3;

    float2 w12 = w1 + w2;

    float2 tc0  = (tc - 1.0)         * invTex;
    float2 tc12 = (tc + w2 / w12)    * invTex;
    float2 tc3  = (tc + 2.0)         * invTex;

    // clang-format off
    float3 r =
        tex.SampleLevel(samp, float2(tc0.x,  tc0.y),  0).rgb * (w0.x  * w0.y)  +
        tex.SampleLevel(samp, float2(tc12.x, tc0.y),  0).rgb * (w12.x * w0.y)  +
        tex.SampleLevel(samp, float2(tc3.x,  tc0.y),  0).rgb * (w3.x  * w0.y)  +
        tex.SampleLevel(samp, float2(tc0.x,  tc12.y), 0).rgb * (w0.x  * w12.y) +
        tex.SampleLevel(samp, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y) +
        tex.SampleLevel(samp, float2(tc3.x,  tc12.y), 0).rgb * (w3.x  * w12.y) +
        tex.SampleLevel(samp, float2(tc0.x,  tc3.y),  0).rgb * (w0.x  * w3.y)  +
        tex.SampleLevel(samp, float2(tc12.x, tc3.y),  0).rgb * (w12.x * w3.y)  +
        tex.SampleLevel(samp, float2(tc3.x,  tc3.y),  0).rgb * (w3.x  * w3.y);
    // clang-format on
    return r;
}

#endif // TAA_HISTORY_HLSLI
