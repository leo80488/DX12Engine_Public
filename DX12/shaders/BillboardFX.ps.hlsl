// BillboardFX.ps.hlsl — bindless sprite-sheet billboard sampler.
//
// Samples the bound atlas page at the CPU-baked sub-rect UV and modulates by
// the per-vertex HDR colour (tint * emissive, alpha = tint.a * opacity).
//   texIdx == 0xFFFFFFFF → untextured: return the colour directly.
// The low 31 bits are the bindless index into the engine's space2 table.

Texture2D    g_BindlessTex[] : register(t0, space2);
SamplerState g_Smp           : register(s0, space0);

struct PSIn
{
    float4 pos                  : SV_POSITION;
    float2 uv                   : TEXCOORD0;
    nointerpolation uint texIdx : TEXCOORD1;
    float4 color                : COLOR0;
};

float4 main(PSIn i) : SV_TARGET
{
    if (i.texIdx == 0xFFFFFFFFu)
        return i.color;                       // untextured — flat colour

    float4 tex = g_BindlessTex[NonUniformResourceIndex(i.texIdx)].Sample(g_Smp, i.uv);
    return tex * i.color;                      // tex × (tint*emissive, opacity)
}
