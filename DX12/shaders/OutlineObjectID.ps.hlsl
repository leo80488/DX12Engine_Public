// OutlineObjectID.ps.hlsl — renders an object ID mask for the outline system.
//
// Each outlined entity writes (meshDescIdx + 1) into a R32_UINT render target.
// The screen-space composite pass reads this to detect edges between different
// objects and draws outlines along those edges.

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint pickingFlag;   // non-zero → OR bit-31 into output so sub-pass 3 picks selection color
};

// Input comes from GBuffer.vs.hlsl (world position / uv not used here).
struct PSIn
{
    float4 sv       : SV_POSITION;
    float3 worldPos : POSITIONWS;
    float2 uv       : TEXCOORD0;
    float3 wn       : NORMAL;
    float3 wt       : TANGENT;
    float3 wbt      : BINORMAL;
    float3 col      : COLOR;
};

uint main(PSIn i) : SV_TARGET
{
    uint id = meshDescIdx + 1u;
    if (pickingFlag != 0u) id |= 0x80000000u;
    return id;
}
