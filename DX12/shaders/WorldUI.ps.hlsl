// WorldUI.ps.hlsl — bindless world-space UI sampler.
// Bar / border verts pass texIdx = 0xFFFFFFFF (sentinel) to short-circuit
// directly to the vertex colour with no texture sample.  Text glyphs and
// images pass their bindless index into the engine's space2 texture table.

Texture2D    g_BindlessTex[] : register(t0, space2);
SamplerState g_UISmp         : register(s0, space0);

struct PSIn
{
    float4 pos                          : SV_POSITION;
    float2 uv                           : TEXCOORD0;
    nointerpolation uint texIdx         : TEXCOORD1;
    float4 color                        : COLOR0;
};

float4 main(PSIn i) : SV_TARGET
{
    if (i.texIdx == 0xFFFFFFFFu)
        return i.color;
    float4 tex = g_BindlessTex[NonUniformResourceIndex(i.texIdx)].Sample(g_UISmp, i.uv);
    return tex * i.color;
}
