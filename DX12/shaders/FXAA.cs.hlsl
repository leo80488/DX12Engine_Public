// FXAA.cs.hlsl — NVIDIA FXAA 3.11 PC quality preset, HDR-aware compute port.
//
// Reference: NVIDIA FXAA 3.11 white paper / Fxaa3_11.h header.
// Algorithm:
//   1. 5-tap luma (centre + NSWE).
//   2. Local-contrast early-out: skip pixels below qualityEdgeThreshold.
//   3. 4 corner samples (NW NE SW SE) for second-derivative edge orientation.
//   4. Pick orientation (horz vs vert) and step direction (toward steeper side).
//   5. Walk along edge in both directions up to 12 taps to find edge endpoints.
//   6. Compute pixel offset from edge endpoint distances.
//   7. Subpixel quality: blend toward the 12-tap luma average for sub-pixel AA.
//   8. Sample at offset position via bilinear; output.
//
// HDR: uses BT.601 luma (matches FXAA reference); HDR contrast is implicitly
// handled by the relative threshold. No tonemap needed.
//
// Root signature (compute space2, matches GraphicsDX12::CreateComputeRootSignature):
//   b0 space2 — FXAACB
//   t0 space2 — input HDR (raw scene or TAA's resolved output)
//   u0 space2 — output UAV (R16G16B16A16_FLOAT)

cbuffer FXAACB : register(b0, space2)
{
    uint   width;
    uint   height;
    float  qualitySubpix;
    float  qualityEdgeThreshold;
    float  qualityEdgeThresholdMin;
    float3 _pad0;
};

Texture2D<float4>   gInput  : register(t0, space2);
RWTexture2D<float4> gOutput : register(u0, space2);
SamplerState        gLinear : register(s0, space2);

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// Edge-walking step distances (NVIDIA's 12-step quality tail, matches FXAA_QUALITY__PRESET 39).
static const float kStepQuality[8] = { 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0 };

[numthreads(8, 8, 1)]
void CSMain(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    float2 invSize = 1.0 / float2(width, height);
    float2 uv      = (float2(id) + 0.5) * invSize;

    // ---- 5-tap centre + cardinals --------------------------------------------
    // Centre sample also captures alpha — TAA stores prev-frame luma there for
    // downstream AutoExposure; forward it unchanged so FXAA+TAA mode keeps
    // exposure tracking correct.
    float4 sampleM = gInput.SampleLevel(gLinear, uv, 0);
    float3 rgbM    = sampleM.rgb;
    float  alphaM  = sampleM.a;
    float lumaM = Luma(rgbM);

    float lumaN = Luma(gInput.SampleLevel(gLinear, uv + float2( 0.0,        -invSize.y), 0).rgb);
    float lumaS = Luma(gInput.SampleLevel(gLinear, uv + float2( 0.0,         invSize.y), 0).rgb);
    float lumaE = Luma(gInput.SampleLevel(gLinear, uv + float2( invSize.x,   0.0      ), 0).rgb);
    float lumaW = Luma(gInput.SampleLevel(gLinear, uv + float2(-invSize.x,   0.0      ), 0).rgb);

    // ---- Local-contrast early-out --------------------------------------------
    float lumaMin   = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax   = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(qualityEdgeThresholdMin, lumaMax * qualityEdgeThreshold))
    {
        gOutput[id] = float4(rgbM, alphaM);
        return;
    }

    // ---- 4 corner samples for edge-orientation discriminator -----------------
    float lumaNW = Luma(gInput.SampleLevel(gLinear, uv + float2(-invSize.x, -invSize.y), 0).rgb);
    float lumaNE = Luma(gInput.SampleLevel(gLinear, uv + float2( invSize.x, -invSize.y), 0).rgb);
    float lumaSW = Luma(gInput.SampleLevel(gLinear, uv + float2(-invSize.x,  invSize.y), 0).rgb);
    float lumaSE = Luma(gInput.SampleLevel(gLinear, uv + float2( invSize.x,  invSize.y), 0).rgb);

    // Sums used by both edge-detection and subpixel-AA stages.
    float lumaDownUp        = lumaN + lumaS;
    float lumaLeftRight     = lumaW + lumaE;
    float lumaLeftCorners   = lumaNW + lumaSW;
    float lumaRightCorners  = lumaNE + lumaSE;
    float lumaTopCorners    = lumaNW + lumaNE;
    float lumaBottomCorners = lumaSW + lumaSE;

    // Second derivatives — accumulate per-axis.
    float edgeHorz = abs((-2.0 * lumaW) + lumaLeftCorners)
                   + abs((-2.0 * lumaM) + lumaDownUp     ) * 2.0
                   + abs((-2.0 * lumaE) + lumaRightCorners);
    float edgeVert = abs((-2.0 * lumaN) + lumaTopCorners)
                   + abs((-2.0 * lumaM) + lumaLeftRight  ) * 2.0
                   + abs((-2.0 * lumaS) + lumaBottomCorners);

    bool isHorzEdge = (edgeHorz >= edgeVert);

    // ---- Pick step direction (toward steepest gradient) ----------------------
    float luma1 = isHorzEdge ? lumaN : lumaW;
    float luma2 = isHorzEdge ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    bool  is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength   = isHorzEdge ? -invSize.y : -invSize.x;
    float lumaLocalAvg = 0.0;
    if (is1Steepest)
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    else
    {
        stepLength   = -stepLength;
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }

    // Shift centre half-pixel toward the steeper neighbour.
    float2 currentUV = uv;
    if (isHorzEdge) currentUV.y += stepLength * 0.5;
    else            currentUV.x += stepLength * 0.5;

    // ---- Walk both directions along the edge ---------------------------------
    float2 offset = isHorzEdge ? float2(invSize.x, 0.0) : float2(0.0, invSize.y);
    float2 uv1 = currentUV - offset;
    float2 uv2 = currentUV + offset;

    float lumaEnd1 = Luma(gInput.SampleLevel(gLinear, uv1, 0).rgb) - lumaLocalAvg;
    float lumaEnd2 = Luma(gInput.SampleLevel(gLinear, uv2, 0).rgb) - lumaLocalAvg;

    bool reached1   = abs(lumaEnd1) >= gradientScaled;
    bool reached2   = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        if (reachedBoth) break;

        if (!reached1) lumaEnd1 = Luma(gInput.SampleLevel(gLinear, uv1, 0).rgb) - lumaLocalAvg;
        if (!reached2) lumaEnd2 = Luma(gInput.SampleLevel(gLinear, uv2, 0).rgb) - lumaLocalAvg;

        reached1 = abs(lumaEnd1) >= gradientScaled;
        reached2 = abs(lumaEnd2) >= gradientScaled;
        reachedBoth = reached1 && reached2;

        if (!reached1) uv1 -= offset * kStepQuality[i];
        if (!reached2) uv2 += offset * kStepQuality[i];
    }

    // ---- Compute pixel offset from end-distance asymmetry --------------------
    float distance1 = isHorzEdge ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float distance2 = isHorzEdge ? (uv2.x - uv.x) : (uv2.y - uv.y);

    bool  isDir1 = distance1 < distance2;
    float distanceFinal  = min(distance1, distance2);
    float edgeThickness  = distance1 + distance2;
    float pixelOffset    = -distanceFinal / max(edgeThickness, 1e-5) + 0.5;

    // Verify the centre is on the correct side of the edge — sign mismatch
    // means the edge ended in the same direction as the centre's luma deviation,
    // so we shouldn't apply any offset.
    bool isCentreSmaller   = lumaM < lumaLocalAvg;
    bool correctVariation  = ((isDir1 ? lumaEnd1 : lumaEnd2) < 0.0) != isCentreSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // ---- Subpixel anti-aliasing ---------------------------------------------
    // Weighted average of 12 samples (cardinals×2 + corners) used as the
    // sub-pixel luma reference. Larger deviation of centre from this reference
    // means the centre likely IS a sub-pixel feature, biasing offset upward.
    float lumaAvg = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight)
                                    + lumaLeftCorners + lumaRightCorners);
    float subPixOffset1 = saturate(abs(lumaAvg - lumaM) / max(lumaRange, 1e-5));
    float subPixOffset2 = (-2.0 * subPixOffset1 + 3.0) * subPixOffset1 * subPixOffset1;
    float subPixOffsetFinal = subPixOffset2 * subPixOffset2 * qualitySubpix;

    finalOffset = max(finalOffset, subPixOffsetFinal);

    // ---- Final bilinear sample at offset position ---------------------------
    float2 finalUV = uv;
    if (isHorzEdge) finalUV.y += finalOffset * stepLength;
    else            finalUV.x += finalOffset * stepLength;

    float3 finalRGB = gInput.SampleLevel(gLinear, finalUV, 0).rgb;
    gOutput[id] = float4(finalRGB, alphaM);  // forward TAA's prev-luma alpha
}
