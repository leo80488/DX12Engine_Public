// UI.ps.hlsl — sample the bound texture (or solid white when untextured) and
// modulate by the per-vertex tint.
//
// Bindings (sharing the engine default root signature):
//   t3 space0 — UI texture (atlas / sprite / font page).
//   s0        — linear-clamp sampler (provided by UIPass via BindSampler).

Texture2D    g_UITex   : register(t3, space0);
SamplerState g_UISmp   : register(s0, space0);

struct PSIn
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(PSIn i) : SV_TARGET
{
    float4 tex = g_UITex.Sample(g_UISmp, i.uv);
    return tex * i.color;
}
