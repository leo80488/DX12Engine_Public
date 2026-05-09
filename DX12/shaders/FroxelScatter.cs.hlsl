// FroxelScatter.cs.hlsl
// -----------------------------------------------------------------------------
// Pass 4 — front-to-back integration along the view-Z axis.
// Each (x, y) thread walks all `froxelD` slices and accumulates:
//   .rgb = ∫ T(s) · L(s) ds        (in-scattered light reaching the camera)
//   .a   = T(zEnd) = exp(−∫ σ_t ds)  (transmittance from camera to this slice)
//
// Apply pass samples this with trilinear and composites:
//   final = scene.rgb × T + scattered.rgb
// -----------------------------------------------------------------------------

#include "FroxelCommon.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture3D<float4>     SrcLighting    : register(t0, space2);
RWTexture3D<float4>   DstScattering  : register(u0, space2);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH) return;

    float3 accum = float3(0, 0, 0);
    float  trans = 1.0;

    // Scatter deliberately does NOT jitter. Adding an XY jitter here made
    // the same column's cells sample different UVs each frame, which broke
    // the monotonically-increasing integral invariant and introduced extra
    // per-frame noise that temporal couldn't cancel. LightInject's jitter
    // already supplies the necessary per-frame variation in the source
    // lighting; we just integrate it faithfully.

    // Walk slices front (z=0 = near) → back. Per-slice integral closed form:
    //   T_i  = exp(−σ_t · Δd_i)
    //   L_i  = σ_s · L_in / σ_t · (1 − T_i)
    // Final accumulated:  L_total = Σ trans · L_i ; trans *= T_i
    [loop] for (uint z = 0; z < P.froxelD; ++z)
    {
        uint3  coord  = uint3(DTid.xy, z);
        float4 cell   = SrcLighting[coord];
        float3 inSctL = cell.rgb;     // already σ_s × L_in
        float  sigmaT = max(cell.a, 1e-6);

        // Slice thickness in world units (next slice depth − this slice depth).
        float dNear = FroxelSliceToViewDepth(float(z),       P.froxelNear, P.froxelFar, P.froxelD);
        float dFar  = FroxelSliceToViewDepth(float(z + 1u),  P.froxelNear, P.froxelFar, P.froxelD);
        float dt    = dFar - dNear;

        float  Ti  = exp(-sigmaT * dt);
        float3 Li  = inSctL * (1.0 - Ti) / sigmaT;

        accum += trans * Li;
        trans *= Ti;

        DstScattering[coord] = float4(accum, trans);
    }
}
