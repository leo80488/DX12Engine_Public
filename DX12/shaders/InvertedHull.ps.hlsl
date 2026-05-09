// InvertedHull.ps.hlsl — pixel shader for the inverted-hull outline pass.
// Alpha fades with distance; beyond fadeEnd the VS already clips the triangle.

cbuffer OutlineCB : register(b2, space0)
{
    float    outlinePixels;
    float    depthThreshold;
    float    normalThreshold;
    float    outlineStrength;
    float3   outlineColor;
    float    outlineFadeStart;
    uint     vpWidth;
    uint     vpHeight;
    float    outlineFadeEnd;
    float    nearZ;
    float    farZ;
};

struct PSIn
{
    float4 sv       : SV_POSITION;
    float  distFade : TEXCOORD0;
};

float4 main(PSIn i) : SV_TARGET
{
    clip(i.distFade - 0.001);
    return float4(outlineColor, i.distFade);
}
