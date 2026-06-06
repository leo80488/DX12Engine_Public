// WorldUI.ps.hlsl -- bindless world-space UI sampler.
//
// Per-vertex texIdx encodes the binding + a kind:
//   0xFFFFFFFF              -- bar / border: short-circuit to the vertex colour.
//   bit31 set (| 0x8000..)  -- SDF font glyph: the bound page's ALPHA is a signed
//                            distance field; reconstruct a crisp edge so text
//                            stays sharp at any distance/scale.
//   bit31 clear            -- plain image: sample * tint (real coverage texture).
// The low 31 bits are always the bindless index into the engine's space2 table.

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
        return i.color;                       // bar / border -- flat colour

    const uint idx   = i.texIdx & 0x7FFFFFFFu;          // strip SDF flag bit
    const bool isSDF = (i.texIdx & 0x80000000u) != 0u;
    float4 tex = g_BindlessTex[NonUniformResourceIndex(idx)].Sample(g_UISmp, i.uv);

    if (isSDF)
    {
        float dist = tex.a;                   // 0.5 == glyph edge
        float aa   = max(fwidth(dist), 1e-4);
        float a    = smoothstep(0.5 - aa, 0.5 + aa, dist);
        return float4(i.color.rgb, i.color.a * a);
    }
    return tex * i.color;                      // plain image
}
