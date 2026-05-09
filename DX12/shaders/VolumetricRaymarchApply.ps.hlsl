// VolumetricRaymarchApply.ps.hlsl
// -----------------------------------------------------------------------------
// Fullscreen triangle composite for the per-pixel volumetric raymarch pass.
// Runs AFTER lighting writes HDR and BEFORE the froxel VolumetricApply. The
// pipeline uses additive (src=ONE, dst=ONE) blending so the raymarched sun
// + shadow-spot in-scatter is layered onto the HDR scene; the subsequent
// froxel apply then extinguishes the combined HDR through the medium's
// transmittance (single source of truth for extinction).
//
// Input:  half-res Texture2D<float4>  (raymarch result after temporal blend)
//         depth buffer (for sky / distance fade checks)
// Output: (inScatter.rgb, 0) — alpha kept zero so extinction is not
//         double-applied by the froxel compositor downstream.
// -----------------------------------------------------------------------------

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

Texture2D<float>   gDepth     : register(t5, space0);
Texture2D<float4>  gRaymarch  : register(t6, space0);  // half-res; bilinear upsample here
SamplerState       gLinear    : register(s0);

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_TARGET
{
    // Bilinear upsample — half-res → full-res. Linear sampler handles the
    // 2×2 tap interpolation for us. For sharp silhouette edges we could
    // add a bilateral / depth-aware weight; the half-res raymarch hides
    // most seams because the in-scatter field is already smooth.
    float4 rm = gRaymarch.SampleLevel(gLinear, i.uv, 0);

    // Keep alpha = 0 so the blend op ignores src-alpha extinction (the
    // downstream froxel apply owns the single medium's transmittance).
    return float4(rm.rgb, 0.0);
}
