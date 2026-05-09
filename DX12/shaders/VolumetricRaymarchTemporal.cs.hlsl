// VolumetricRaymarchTemporal.cs.hlsl
// -----------------------------------------------------------------------------
// Temporal reprojection for the half-res per-pixel volumetric raymarch.
// Matches the structure of FroxelTemporal.cs.hlsl but works in 2D screen
// space against the scene-depth reconstructed world position.
//
//   Current : RWTexture2D<float4> written by VolumetricRaymarch (this frame)
//   History : Texture2D<float4>  — previous frame's blended result
//   Output  : RWTexture2D<float4> — this frame's blended result (= next frame's history)
//
// Reprojection strategy:
//   * Reconstruct the surface world pos from scene depth at the half-res
//     pixel's full-res UV. (The raymarch output approximates radiance toward
//     that point, so using the surface as the reprojection anchor gives the
//     correct motion under camera rotation / translation.)
//   * Push through prevViewProj → previous UV.
//   * 3×3 neighbourhood clamp in current-frame space to kill ghosting.
//   * lerp(history, current, temporalAlpha).
// -----------------------------------------------------------------------------

#include "FroxelCommon.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture2D<float4>   CurrentSRV  : register(t0, space2);
Texture2D<float4>   HistorySRV  : register(t1, space2);
Texture2D<float>    SceneDepth  : register(t3, space2);
SamplerState        LinearSampler : register(s0, space2);

RWTexture2D<float4> OutputUAV   : register(u0, space2);

// Convert half-res UV + scene-depth ndc to a world position. Uses the same
// reversed-Z finite-far formula as VolumetricRaymarch::BuildViewRay.
float3 ReconstructWorldPos(float2 uv, float ndcZ)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 h   = mul(float4(ndc, ndcZ, 1.0), P.invViewProj);
    return h.xyz / h.w;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint halfW, halfH;
    OutputUAV.GetDimensions(halfW, halfH);
    if (DTid.x >= halfW || DTid.y >= halfH) return;

    float4 current = CurrentSRV[DTid.xy];
    float2 uv      = (float2(DTid.xy) + 0.5) / float2(halfW, halfH);

    // Sample depth at the half-res UV (centre of a 2×2 full-res block).
    float ndcZ = SceneDepth.SampleLevel(LinearSampler, uv, 0).r;

    // Sky / empty → skip reprojection entirely; the raymarch output has no
    // surface anchor to reproject against. Write current as-is.
    if (ndcZ <= 1e-6)
    {
        OutputUAV[DTid.xy] = current;
        return;
    }

    float3 worldPos = ReconstructWorldPos(uv, ndcZ);

    // Project into previous frame.
    float4 prevClip = mul(float4(worldPos, 1.0), P.prevViewProj);
    if (prevClip.w <= 0.0)
    {
        OutputUAV[DTid.xy] = current;
        return;
    }
    float2 prevUV = float2( prevClip.x / prevClip.w * 0.5 + 0.5,
                           -prevClip.y / prevClip.w * 0.5 + 0.5);
    if (any(prevUV < 0.0) || any(prevUV > 1.0))
    {
        OutputUAV[DTid.xy] = current;
        return;
    }

    float4 history = HistorySRV.SampleLevel(LinearSampler, prevUV, 0);

    // Neighbourhood clamp (3×3). Keeps history bounded to the plausible
    // per-pixel distribution → disocclusion events reject history cleanly.
    float4 nMin = current, nMax = current;
    int2 dim = int2(halfW, halfH) - 1;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0) continue;
        int2   nc = clamp(int2(DTid.xy) + int2(dx, dy), int2(0, 0), dim);
        float4 n  = CurrentSRV[nc];
        nMin = min(nMin, n);
        nMax = max(nMax, n);
    }
    history = clamp(history, nMin, nMax);

    // lerp(history, current, alpha). temporalAlpha plumbed through the same
    // CB field as the froxel temporal pass.
    float4 result = lerp(history, current, P.temporalAlpha);
    OutputUAV[DTid.xy] = result;
}
