// CloudComposite.ps.hlsl
// -----------------------------------------------------------------------------
// Bilinear upsample the quarter-resolution cloud raymarch result and emit it
// in premultiplied form (rgb = pre-multiplied scatter, a = transmittance).
//
// Blend state set on the PSO is:
//   out = src.rgb + dst.rgb * src.a
//        = scatter + sceneRgb * transmittance
// so a transmittance of 1.0 (clear sky) leaves the scene untouched, and
// transmittance of 0.0 fully replaces the scene with cloud scatter.
// -----------------------------------------------------------------------------

Texture2D<float4>  CloudHalfRes : register(t6, space0);
SamplerState       LinearClamp  : register(s0, space0);

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target
{
    // Bilinear upsample. The raymarch wrote premultiplied scatter + remaining
    // transmittance; the blend state expects exactly this format.
    return CloudHalfRes.SampleLevel(LinearClamp, i.uv, 0);
}
