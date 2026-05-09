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
//   (B) Tent-filtered de-jittered current sample in HDR space. The projection
//       matrix was offset by (jitterX, jitterY) pixels this frame; the tent
//       kernel reconstructs the unjittered pixel value at the centre of the
//       output texel, sharing the same Karis luma weight to suppress fireflies
//       in the reconstructed signal.
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
    float  centerY;                 // YCoCg.x of the centre tap (same space as m1) —
                                    // used by Fix E v2's bimodal fence detector
    float3 meanRGB_HDR_unweighted;  // 1/9 unweighted HDR mean — Fix K's
                                    // soft clamp wants the spatial average
                                    // unbiased by Karis weighting
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

            // Centre tap's Y in the same space as m1 — Fix E v2 reads this
            // to test bimodality (centre vs neighbourhood mean / sigma).
            if (dx == 0 && dy == 0)
                centerY = ycocg.x;

            m1     += ycocg * lumaW;
            m2     += ycocg * ycocg * lumaW;
            statsW += lumaW;

            // (B) Tent de-jitter weight, also Karis-weighted
            float wx = max(0.0, 1.0 - abs(float(dx) - jitX));
            float wy = max(0.0, 1.0 - abs(float(dy) - jitY));
            float w  = wx * wy * lumaW;
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
