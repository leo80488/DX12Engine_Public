// UI.ps.hlsl -- UI pixel shader.
//
// Two materials, selected by the b0 root constant the UIPass sets per draw:
//   materialID 0 -- plain: sample the bound texture * per-vertex tint
//                  (images, sprites, solid-colour fills via the 1x1 white tex).
//   materialID 1 -- SDF text: the bound texture's ALPHA channel is a signed
//                  distance field (0.5 == glyph edge). Reconstruct a crisp edge
//                  with smoothstep over the screen-space derivative so glyphs
//                  stay sharp at ANY scale, and apply optional outline / glow
//                  from the effects table (b2), indexed by the b0 effectIndex.
//
// Bindings (engine default root signature):
//   b0 space0 -- UIDrawConsts (materialID, effectIndex), set per draw command.
//   b2 space0 -- UIEffects table (uploaded from UIDrawList::Effects()).
//   t3 space0 -- UI texture (atlas / sprite / SDF font page).
//   s0        -- linear-clamp sampler (REQUIRED for SDF AA).

Texture2D    g_UITex : register(t3, space0);
SamplerState g_UISmp : register(s0, space0);

cbuffer UIDrawConsts : register(b0, space0)
{
    uint g_materialID;   // 0 = plain, 1 = SDF text
    uint g_effectIndex;  // index into g_fx[]
    uint g_uiPad0;
    uint g_uiPad1;
};

struct UITextEffect
{
    float4 outlineColor;
    float4 glowColor;
    float  outlineWidthN; // normalised (px / sdfPixelRange); 0 = no outline
    float  glowWidthN;    // normalised;                      0 = no glow
    float  softness;
    float  _pad;
};
cbuffer UIEffects : register(b2, space0)
{
    UITextEffect g_fx[64];
};

struct PSIn
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(PSIn i) : SV_TARGET
{
    float4 tex = g_UITex.Sample(g_UISmp, i.uv);

    if (g_materialID == 0u)
        return tex * i.color;            // plain image / solid fill

    // ---- SDF text ------------------------------------------------------------
    float dist = tex.a;                  // 0.5 == glyph edge
    float aa   = max(fwidth(dist), 1e-4); // screen-space AA half-width
    float fill = smoothstep(0.5 - aa, 0.5 + aa, dist);

    UITextEffect fx = g_fx[min(g_effectIndex, 63u)];

    float3 rgb = i.color.rgb;
    float  a   = i.color.a * fill;

    // Outline -- a band just outside the glyph body.
    if (fx.outlineWidthN > 0.0)
    {
        float outlineA = smoothstep(0.5 - fx.outlineWidthN - aa,
                                    0.5 - fx.outlineWidthN + aa, dist) * fx.outlineColor.a;
        rgb = lerp(fx.outlineColor.rgb, i.color.rgb, fill);
        a   = max(i.color.a * fill, outlineA);
    }

    // Glow -- soft halo fading outward from the edge, composited under the glyph.
    if (fx.glowWidthN > 0.0)
    {
        float glowA = smoothstep(0.5 - fx.glowWidthN, 0.5, dist) * fx.glowColor.a;
        rgb = lerp(fx.glowColor.rgb, rgb, a);
        a   = max(a, glowA);
    }

    return float4(rgb, a);
}
