// VolumetricApply.ps.hlsl
// -----------------------------------------------------------------------------
// Composite pass for the volumetric fog froxel pipeline.
// Runs as a fullscreen triangle drawn AFTER the lighting pass writes HDR.
//
// Input  : 3D scattering texture (xy = screen UV, z = exponential view-Z slice)
//          Depth buffer (per-pixel NDC z)
// Output : (scattering.rgb, 1 - transmittance)
//          With src=ONE / dst=INV_SRC_ALPHA blending this resolves to
//             final = scattering + scene * transmittance
//
// IMPORTANT: this PS uses the engine's GRAPHICS root signature only — no
// "space2" registers. CB lives at b3 space0 (free CBV slot). t5 is the
// existing GBuffer depth slot; t6 is the skybox-env-cube slot which we
// re-purpose to bind the Texture3D scattering SRV during this draw call.
// -----------------------------------------------------------------------------

#include "DepthCommon.hlsli"

cbuffer VolApplyCB : register(b3, space0)
{
    float NearZ;
    float FarZ;
    float FroxelNear;
    float FroxelFar;
    uint  FroxelDepth;
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

Texture2D<float>   gDepth      : register(t5, space0);
Texture3D<float4>  gScattering : register(t6, space0);
SamplerState       gLinear     : register(s0);

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// Cheap interleaved-gradient noise from Jorge Jimenez — screen-space dither
// used to break up the discrete Z-slice boundaries in the froxel grid so
// the composite reads as a smooth gradient instead of visible rings.
float IGN(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float4 main(PSIn i) : SV_TARGET
{
    float ndcZ = gDepth.SampleLevel(gLinear, i.uv, 0).r;
    if (ndcZ <= 0.0001) return float4(0, 0, 0, 0);   // sky (reversed Z: far=0)

    float viewZ = DepthToViewZ(ndcZ, NearZ, FarZ);
    if (viewZ <= FroxelNear) return float4(0, 0, 0, 0);

    float  w   = ViewZToFroxelW(viewZ, FroxelNear, FroxelFar);

    // Offset sliceT by ±0.5 of one slice using a screen-space hash. Linear
    // trilinear then averages two adjacent slices per pixel in a random
    // pattern — the hard horizontal rings on spot cones disappear and the
    // result looks smoothly shaded.
    float dither = (IGN(i.pos.xy) - 0.5) / float(FroxelDepth);
    w = saturate(w + dither);

    float3 uvw = float3(i.uv.x, i.uv.y, w);

    float4 scat = gScattering.SampleLevel(gLinear, uvw, 0);
    return float4(scat.rgb, 1.0 - scat.a);
}
