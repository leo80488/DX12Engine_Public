#ifndef TAA_NEIGHBOURHOOD_HLSLI
#define TAA_NEIGHBOURHOOD_HLSLI

#include "TAA_Common.hlsli"

// -----------------------------------------------------------------------------
// TAA_Neighbourhood.hlsli
//
// Combined 3x3 traversal that produces, in a single pass:
//
//   (A) Karis luma-weighted AABB statistics (mean + variance) in tonemapped
//       YCoCg space. Each sample contributes weight 1/(1+Luma(s)), so a single
//       firefly outlier cannot blow the AABB wide open and let history
//       fireflies survive the variance clip.
//
//   (B) Blackman-Harris de-jittered current sample in HDR space (Karis 2014).
//       The projection matrix was offset by (jitterX, jitterY) pixels this
//       frame; the BH Gaussian kernel exp(-2.29*d^2) reconstructs the
//       unjittered pixel value at the centre of the output texel, sharing the
//       same Karis luma weight to suppress fireflies in the reconstructed
//       signal. BH replaced the older separable tent on 2026-05-20 — tent's
//       wide footprint low-passed every frame and produced visibly softer
//       textures than no-AA reference.
//
//   (C) HDR-space neighbourhood mean (for downstream firefly clamp / bright-
//       peak detection) — derived from m1 by inverse-tonemap.
// -----------------------------------------------------------------------------

struct NeighbourhoodStats
{
    float3 m1;                      // YCoCg mean (Karis-weighted; tonemapped if TAA_USE_TONEMAP_BLEND)
    float3 sigma;                   // per-channel stddev (matching m1's space)
    float3 currFilt;                // de-jittered current sample (always HDR linear)
    float3 meanRGB_HDR;             // m1 returned to HDR linear RGB (for firefly clamp)
    float  centerY;                 // YCoCg.x of the centre tap (same space as m1)
    float  yMin;                    // unweighted min of YCoCg.x over 3x3 (same space as m1) —
                                    // symmetric bimodal detector reference (vs Karis-biased m1.x)
    float  yMax;                    // unweighted max of YCoCg.x over 3x3 (same space as m1)
    float  lumaMinHDR;              // unweighted min of HDR-linear Luma over 3x3 — tonemap-
                                    // invariant contrast metric (max-min)/mid for HDR-bright
                                    // bimodal where tonemapped σ is structurally compressed
    float  lumaMaxHDR;              // unweighted max of HDR-linear Luma over 3x3
    float3 meanRGB_HDR_unweighted;  // 1/9 unweighted HDR mean — Fix K's soft clamp reference
    float3 cardinalSum;             // sum of 4 cardinal HDR taps (top+bottom+left+right) —
                                    // (#4) Karis 5-tap unsharp uses this:
                                    //   sharp = curr*5 - cardinalSum
                                    // Returned raw (not averaged) so the unsharp
                                    // formula stays clean.
    float3 centerHDR;               // raw centre tap in HDR linear (also used by
                                    // Fix G's high-freq-static path; previously
                                    // re-Loaded in TAA.cs.hlsl, now hoisted here
                                    // since the loop already touched it).
};

NeighbourhoodStats ComputeNeighbourhood(Texture2D<float4> currHDR,
                                         int2  pos,
                                         int2  dim,
                                         float jitX,
                                         float jitY)
{
    float3 m1     = 0.0;
    float3 m2     = 0.0;
    float  statsW = 0.0;

    float3 currFilt = 0.0;
    float  currW    = 0.0;

    float3 unweightedSum = 0.0;
    float  centerY       = 0.0;
    float  yMin          =  1e30;
    float  yMax          = -1e30;
    float  lumaMinHDR    =  1e30;
    float  lumaMaxHDR    = -1e30;
    float3 cardinalSum   = 0.0;
    float3 centerHDR     = 0.0;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 sp = clamp(pos + int2(dx, dy), int2(0, 0), dim - 1);
            float3 s = currHDR.Load(int3(sp, 0)).rgb;

            unweightedSum += s;

            // HDR-linear luma min/max (tonemap-invariant) — kept in HDR space
            // regardless of TAA_USE_TONEMAP_BLEND, used by HDR contrast gate.
            {
                float lumaH = Luma(s);
                lumaMinHDR  = min(lumaMinHDR, lumaH);
                lumaMaxHDR  = max(lumaMaxHDR, lumaH);
            }

            // Cardinals only (4-tap cross): used by Karis 5-tap unsharp downstream.
            if ((dx == 0) ^ (dy == 0))
                cardinalSum += s;
            if (dx == 0 && dy == 0)
                centerHDR = s;

            // (A) Karis luma weight — suppresses firefly influence on AABB.
            // In TAA_USE_TONEMAP_BLEND mode the stats live in tonemapped
            // YCoCg space; the weight is taken on the SAME tonemapped luma so
            // its dynamic range stays in [0.5, 1.0] (HDR luma weighting
            // collapses to ~0.02 for bright pixels, which over-deweights
            // legitimate specular highlights and shrinks the AABB).
#if TAA_USE_TONEMAP_BLEND
            float3 sT    = ToneMapLuma(s);
            float  lumaW = 1.0 / (1.0 + Luma(sT));
            float3 ycocg = RGBToYCoCg(sT);
#else
            float  lumaW = 1.0 / (1.0 + Luma(s));
            float3 ycocg = RGBToYCoCg(s);
#endif

            // Centre tap's Y in the same space as m1 — for symmetric bimodal
            // detector (centre vs neighbourhood extremes).
            if (dx == 0 && dy == 0)
                centerY = ycocg.x;

            // Unweighted Y min/max — symmetric bimodal detector reference;
            // Karis-weighted m1.x is biased toward dim values.
            yMin = min(yMin, ycocg.x);
            yMax = max(yMax, ycocg.x);

            m1     += ycocg * lumaW;
            m2     += ycocg * ycocg * lumaW;
            statsW += lumaW;

            // (B) Karis 2014 Blackman-Harris reconstruction (Gaussian approx).
            //
            // Replaced separable tent on 2026-05-20 to address TAA-induced
            // static softness. The tent kernel
            //     w = max(0, 1-|dx-jX|) * max(0, 1-|dy-jY|)
            // spreads weight uniformly over the 3x3 footprint and is the
            // softest separable filter; integrated over many frames it
            // produced visibly low-passed textures relative to no-AA.
            //
            // Blackman-Harris (Karis "High Quality Temporal Supersampling"
            // SIGGRAPH 2014) uses a Gaussian approximation
            //     w = exp(-2.29 * d^2)
            // where d is the (jitter-corrected) distance from sample to
            // pixel center. Energy is concentrated at the centre tap
            // (no-jitter weights: centre=1.0, cardinal=0.101, corner=0.010
            // vs tent: centre=1.0, cardinal=0.0, corner=0.0). With jitter
            // applied the filter still cleanly reconstructs the unjittered
            // signal at the texel centre but without the tent's broad
            // averaging — single-frame HDR sub-pixel peaks stay sharp at
            // their centre tap instead of being spread across neighbours,
            // which also reduces specular shimmer (peak no longer "walks"
            // across the 3x3 footprint as jitter rotates).
            float2 dpos = float2(float(dx) - jitX, float(dy) - jitY);
            float  w    = exp(-2.29 * dot(dpos, dpos)) * lumaW;
            currFilt += s * w;
            currW    += w;
        }
    }

    NeighbourhoodStats st;
    st.m1                       = m1 / statsW;
    float3 m2n                  = m2 / statsW;
    st.sigma                    = sqrt(max(0.0, m2n - st.m1 * st.m1));
    st.currFilt                 = currFilt / max(currW, 1e-5);
    st.centerY                  = centerY;
    st.yMin                     = yMin;
    st.yMax                     = yMax;
    st.lumaMinHDR               = lumaMinHDR;
    st.lumaMaxHDR               = lumaMaxHDR;
    st.meanRGB_HDR_unweighted   = unweightedSum * (1.0 / 9.0);
    st.cardinalSum              = cardinalSum;
    st.centerHDR                = centerHDR;
#if TAA_USE_TONEMAP_BLEND
    st.meanRGB_HDR = InvToneMapLuma(YCoCgToRGB(st.m1));
#else
    st.meanRGB_HDR = YCoCgToRGB(st.m1);
#endif
    return st;
}

#endif // TAA_NEIGHBOURHOOD_HLSLI
