// XeGTAODenoise.cs.hlsl — VERBATIM (structural) port of Intel XeGTAO_Denoise.
//
// Per-invocation, this thread denoises TWO horizontally-adjacent pixels —
// dispatch grid is therefore (width/2, height) tiles of 8×8 threads each.
// Base pixel = dispatchThreadID * uint2(2, 1).
//
// Algorithm (from Intel XeGTAO.hlsli :: XeGTAO_Denoise):
//   1. Read the 5 edges needed (centre + L/R/T/B) and AO for the 3×3 block
//      around each side-pixel.
//   2. Combine centre.LRTB with neighbour edges to form a symmetric edge
//      weight per cardinal direction.
//   3. Add a "leak compensation" term that re-introduces some blur across
//      near-edges so thin edges don't stop all denoising.
//   4. Diagonal weights = diagWeight * path-product of two cardinal edges.
//   5. Weighted sum of 8 neighbours + centre, normalised by total weight.
//
// Compute root signature (space2):
//   b0 — DenoiseCB
//   t0 — raw AO input  (R8_UNORM)   from main pass
//   t1 — packed edges  (R8_UNORM)   from main pass
//   u0 — denoised AO   (R8_UNORM)

cbuffer DenoiseCB : register(b0, space2)
{
    uint   viewportWidth;
    uint   viewportHeight;
    float  depthLinearizeMul;    // kept for future compatibility; unused
    float  depthLinearizeAdd;
    float  denoiseBlurBeta;      // user-tunable (XE_GTAO_DEFAULT = 1.2)
    uint   finalApply;           // 0 = intermediate pass (β / 5); 1 = last pass (β)
    float2 _pad;
};

Texture2D<float>   gAOInput : register(t0, space2);
Texture2D<float>   gEdges   : register(t1, space2);
RWTexture2D<float> gOutput  : register(u0, space2);
SamplerState       gLinear  : register(s0, space2);

// ---------------------------------------------------------------------------
// XeGTAO_UnpackEdges — VERBATIM from reference.
// ---------------------------------------------------------------------------
float4 XeGTAO_UnpackEdges(float packedVal)
{
    uint pv = uint(packedVal * 255.5);
    float4 edgesLRTB;
    edgesLRTB.x = float((pv >> 6) & 0x03u) / 3.0;
    edgesLRTB.y = float((pv >> 4) & 0x03u) / 3.0;
    edgesLRTB.z = float((pv >> 2) & 0x03u) / 3.0;
    edgesLRTB.w = float((pv >> 0) & 0x03u) / 3.0;
    return saturate(edgesLRTB);
}

// Helper — clamped load with edge replication.
float LoadClampAO(int2 px)
{
    px = clamp(px, int2(0, 0), int2(int(viewportWidth) - 1, int(viewportHeight) - 1));
    return gAOInput.Load(int3(px, 0));
}
float LoadClampEdgesPacked(int2 px)
{
    px = clamp(px, int2(0, 0), int2(int(viewportWidth) - 1, int(viewportHeight) - 1));
    return gEdges.Load(int3(px, 0));
}

// ---------------------------------------------------------------------------
// Per-pixel denoise — structurally identical to the reference inner loop.
// ---------------------------------------------------------------------------
void DenoisePixel(int2 pixCoord, float blurAmount)
{
    const float diagWeight = 0.85 * 0.5;

    // Centre edges (and the 4 cardinal neighbours' edges, for the symmetric
    // weight + diagonal path-products).
    float4 edgesC_LRTB = XeGTAO_UnpackEdges(LoadClampEdgesPacked(pixCoord));
    float4 edgesL_LRTB = XeGTAO_UnpackEdges(LoadClampEdgesPacked(pixCoord + int2(-1, 0)));
    float4 edgesR_LRTB = XeGTAO_UnpackEdges(LoadClampEdgesPacked(pixCoord + int2( 1, 0)));
    float4 edgesT_LRTB = XeGTAO_UnpackEdges(LoadClampEdgesPacked(pixCoord + int2( 0,-1)));
    float4 edgesB_LRTB = XeGTAO_UnpackEdges(LoadClampEdgesPacked(pixCoord + int2( 0, 1)));

    // Make the centre-to-neighbour weight symmetric (centre's "I have an
    // edge to my right" * neighbour's "I have an edge to my left").
    edgesC_LRTB *= float4(edgesL_LRTB.y, edgesR_LRTB.x, edgesT_LRTB.w, edgesB_LRTB.z);

    // Leak compensation — reference's "fill-in blur" for thin edges:
    // re-introduce some blur when the centre has lots of strong edges.
    const float leak_threshold = 2.5;
    const float leak_strength  = 0.5;
    float edginess = (saturate(4.0 - leak_threshold - dot(edgesC_LRTB, 1.0.xxxx))
                      / (4.0 - leak_threshold)) * leak_strength;
    edgesC_LRTB = saturate(edgesC_LRTB + edginess);

    // Diagonal weights = path-product of two cardinal edges meeting at that
    // diagonal (corner pixel shares an edge with both N and E of us, etc.).
    float weightTL = diagWeight * (edgesC_LRTB.x * edgesL_LRTB.z + edgesC_LRTB.z * edgesT_LRTB.x);
    float weightTR = diagWeight * (edgesC_LRTB.z * edgesT_LRTB.y + edgesC_LRTB.y * edgesR_LRTB.z);
    float weightBL = diagWeight * (edgesC_LRTB.w * edgesB_LRTB.x + edgesC_LRTB.x * edgesL_LRTB.w);
    float weightBR = diagWeight * (edgesC_LRTB.y * edgesR_LRTB.w + edgesC_LRTB.w * edgesB_LRTB.y);

    // Load the 3×3 AO values.
    float ssaoC  = LoadClampAO(pixCoord);
    float ssaoL  = LoadClampAO(pixCoord + int2(-1,  0));
    float ssaoR  = LoadClampAO(pixCoord + int2( 1,  0));
    float ssaoT  = LoadClampAO(pixCoord + int2( 0, -1));
    float ssaoB  = LoadClampAO(pixCoord + int2( 0,  1));
    float ssaoTL = LoadClampAO(pixCoord + int2(-1, -1));
    float ssaoTR = LoadClampAO(pixCoord + int2( 1, -1));
    float ssaoBL = LoadClampAO(pixCoord + int2(-1,  1));
    float ssaoBR = LoadClampAO(pixCoord + int2( 1,  1));

    // Weighted sum — edges gate cardinals; diagonals use path weights.
    float sumWeight = blurAmount;
    float sum       = ssaoC * sumWeight;

    sum += ssaoL  * edgesC_LRTB.x; sumWeight += edgesC_LRTB.x;
    sum += ssaoR  * edgesC_LRTB.y; sumWeight += edgesC_LRTB.y;
    sum += ssaoT  * edgesC_LRTB.z; sumWeight += edgesC_LRTB.z;
    sum += ssaoB  * edgesC_LRTB.w; sumWeight += edgesC_LRTB.w;
    sum += ssaoTL * weightTL;      sumWeight += weightTL;
    sum += ssaoTR * weightTR;      sumWeight += weightTR;
    sum += ssaoBL * weightBL;      sumWeight += weightBL;
    sum += ssaoBR * weightBR;      sumWeight += weightBR;

    float result = (sumWeight > 1e-6) ? (sum / sumWeight) : ssaoC;
    gOutput[pixCoord] = result;
}

// ---------------------------------------------------------------------------
// Entry — 2-pixels-per-thread.
// Dispatch: ((viewportWidth + 1) / 2 + 7) / 8  ,  (viewportHeight + 7) / 8.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    // Intermediate pass: blur β / 5  (softer, so the LAST pass does the
    // visible work). Final pass: full β. Matches reference's CSDenoisePass vs
    // CSDenoiseLastPass permutation.
    const float blurAmount = (finalApply != 0u)
        ? denoiseBlurBeta
        : (denoiseBlurBeta / 5.0);

    const uint2 pixCoordBase = dtid.xy * uint2(2u, 1u);

    [unroll] for (uint side = 0; side < 2u; ++side)
    {
        const int2 px = int2(int(pixCoordBase.x + side), int(pixCoordBase.y));
        if (px.x < int(viewportWidth) && px.y < int(viewportHeight))
            DenoisePixel(px, blurAmount);
    }
}
