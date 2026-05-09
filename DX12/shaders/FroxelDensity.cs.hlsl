// FroxelDensity.cs.hlsl
// -----------------------------------------------------------------------------
// Pass 2 — fills the 3D density texture with per-froxel medium parameters.
//   .rgb = scattering coefficient (per channel for chromatic fog)
//   .a   = extinction coefficient (scattering + absorption)
// Includes a simple exponential height-fog so the fog naturally fades with
// altitude.
// -----------------------------------------------------------------------------

#include "FroxelCommon.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

RWTexture3D<float4> DstDensity : register(u0, space2);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH || DTid.z >= P.froxelD)
        return;

    // XY-only sub-voxel jitter — the height-fog boundary would otherwise
    // snap to voxel centres and alias visibly. Z is NOT jittered because
    // far-end voxels are metres wide and a per-frame Z shift shows up as
    // depth flicker that temporal reprojection can't fully hide.
    float3 jitter  = GetFroxelJitter(P.frameIndex);
    jitter.z = 0.0;
    float3 worldPos = FroxelToWorldJittered(DTid, P, jitter);

    // Exponential height fog: density = base * exp(-(y - start) * falloff)
    // Higher altitudes → less fog; below `start` → constant base.
    float h = max(worldPos.y - P.heightFogStart, 0.0);
    float heightFactor = exp(-h * P.heightFogFalloff);

    float density = P.fogDensity * heightFactor;
    float scatter = density * P.fogScattering;
    float absorp  = density * P.fogAbsorption;
    float ext     = scatter + absorp;

    DstDensity[DTid] = float4(scatter.xxx, ext);
}
