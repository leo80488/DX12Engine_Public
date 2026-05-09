// Particle.ps.hlsl — radial falloff + optional texture + per-particle visual mode.
//
// Visual modes:
//   Flat     — pure color * radial alpha (MVP default)
//   Fire     — heat-ramp palette driven by lifeT and radius
//   Smoke    — desaturated color, squared alpha falloff for plumey look
//   Electric — high-contrast pulse with time-based flicker noise
//
// Texture sampling:
//   When texIdx != 0xFFFFFFFFu we sample g_AllTextures[texIdx] and modulate
//   the final color. Texture is treated as an alpha mask on top of whatever
//   the visual mode produced, so a PNG of a smoke puff will shape the
//   particle correctly regardless of which mode is selected.

#include "Particle.hlsli"

// Bindless texture table lives at t0 space2 in the graphics root sig
// (shared with GBufferPass / TransparentPass).
Texture2D<float4> g_AllTextures[] : register(t0, space2);
SamplerState      gLinear          : register(s0, space0);

struct VSOut
{
    float4 pos     : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float4 color   : COLOR0;
    nointerpolation uint texIdx  : TEXCOORD1;
    nointerpolation uint visMode : TEXCOORD2;
    float  lifeT   : TEXCOORD3;
};

// --- Color palettes ---------------------------------------------------------
float3 FirePalette(float t)
{
    // Interpolate black → red → orange → yellow → white across lifeT.
    // Keeps the particle reading as "bright at birth, cooling to ember".
    float3 stops[5] = {
        float3(0.03, 0.02, 0.01),  // dying ember
        float3(1.0,  0.15, 0.02),
        float3(1.0,  0.55, 0.10),
        float3(1.0,  0.88, 0.40),
        float3(1.0,  1.0,  0.95)   // hottest
    };
    float f = saturate(1.0 - t) * 4.0;
    int i = (int)floor(f);
    float blend = f - float(i);
    int hi = min(i + 1, 4);
    return lerp(stops[i], stops[hi], blend);
}

// Cheap value noise for electric flicker (no texture required).
float Hash12(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float4 main(VSOut i) : SV_TARGET
{
    // Radial distance from quad centre (0 at centre, 1 at corners).
    float2 d = i.uv - 0.5;
    float  r = saturate(length(d) * 2.0);

    // Baseline soft-circle mask (same as v1).
    float alpha = 1.0 - smoothstep(0.5, 1.0, r);

    float3 rgb = i.color.rgb;

    // Visual mode overrides / decorates the base color.
    switch (i.visMode)
    {
    case PARTICLE_VISUAL_FIRE:
    {
        // Emissive fire palette; discard baseline color tint and drive from
        // lifeT so the particle sweeps heat→ember over its life.
        rgb = FirePalette(i.lifeT);
        // Squared edge so fire looks sharper at the core.
        alpha = pow(alpha, 0.6);
        break;
    }
    case PARTICLE_VISUAL_SMOKE:
    {
        // Desaturate toward mid-grey and soften the falloff.
        float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(rgb, float3(luma, luma, luma), 0.6);
        alpha *= alpha;  // squared falloff for plumey edges
        break;
    }
    case PARTICLE_VISUAL_ELECTRIC:
    {
        // High-contrast blue-white with time-based flicker on the alpha.
        float flicker = 0.7 + 0.3 * Hash12(i.uv * 17.0 + i.lifeT * 31.0);
        rgb = lerp(rgb, float3(0.6, 0.8, 1.8), 0.7) * flicker * 1.6;
        // Crisp edge — electric arcs don't feather.
        alpha = smoothstep(0.45, 0.3, r);
        break;
    }
    default: // PARTICLE_VISUAL_FLAT
        break;
    }

    // Texture modulation (bindless) — applied on top of the visual-mode output.
    if (i.texIdx != 0xFFFFFFFFu)
    {
        float4 tex = g_AllTextures[NonUniformResourceIndex(i.texIdx)]
                         .SampleLevel(gLinear, i.uv, 0);
        rgb   *= tex.rgb;
        alpha *= tex.a;
    }

    float4 outColor = float4(rgb, i.color.a * alpha);
    if (outColor.a < 0.004) discard;
    return outColor;
}
