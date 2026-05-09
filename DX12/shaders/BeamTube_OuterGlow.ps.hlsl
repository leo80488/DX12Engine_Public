// BeamTube_OuterGlow.ps.hlsl — Transparent additive custom PS for the outer
// glow shell of a procedural-tube heavy beam. Drop-in for
// MaterialComponent.customShaderPath with userBlendMode = Additive.
//
// Output is a single HDR colour; PSO blend = ONE/ONE additive (engine
// permutation ADDITIVE_BLEND). Pre-multiplied: rgb already contains the
// energy contribution; alpha is the fade envelope.
//
// Visual recipe:
//   color = GlowColor * Intensity * scrollNoise(uv.y, time) * fresnelRim(uv.x)
//   alpha = headTailFade(uv.y) * silhouetteFade(uv.x)

#include "material.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

StructuredBuffer<MaterialGPUData> g_Materials   : register(t2, space0);
Texture2D                         g_AllTextures[] : register(t0, space2);
SamplerState                      g_LinearWrap   : register(s0, space0);

cbuffer BeamGlowParams : register(b8, space0)
{
    float4 GlowColor;     // .rgb HDR
    float  Intensity;
    float  RimPower;       // silhouette emphasis
    float  NoiseTiling;
    float  ScrollSpeed;
    float  NoiseFloor;
    float  HeadTailFade;   // smooth-step range at uv.y=0 / uv.y=1
    float  _pad0;
    float  _pad1;
};

Texture2D NoiseTex : register(t0, space3);

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
};

float Hash12(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float ProceduralNoise1D(float u, float scrollOffset)
{
    float fp = u + scrollOffset;
    float i  = floor(fp);
    float f  = frac(fp);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(Hash12(float2(i, 7.0)), Hash12(float2(i + 1.0, 7.0)), f);
}

float4 main(PSIn i) : SV_TARGET
{
    // ---- Silhouette / rim factor -------------------------------------------
    // Outer glow is brightest at the silhouette edges of the tube (where the
    // viewer sees light passing through the most depth). uv.x = 0.5 is the
    // tube centre as seen from the camera; 0 / 1 are the ring edges.
    float side = abs(i.uv.x - 0.5) * 2.0;       // 0 centre, 1 silhouette
    float rim  = pow(saturate(side), max(RimPower, 0.001));

    // ---- Scrolling noise along the beam axis -------------------------------
    float u = i.uv.y * max(NoiseTiling, 0.001) - ScrollSpeed * 0.0;
    float n;
    if (any(NoiseTex.SampleLevel(g_LinearWrap, float2(0.5, 0.5), 0).rgb))
    {
        float2 nuv = float2(i.uv.x, u);
        n = NoiseTex.SampleLevel(g_LinearWrap, nuv, 0).r;
    }
    else
    {
        n = ProceduralNoise1D(u, 0.0);
    }
    n = lerp(saturate(NoiseFloor), 1.0, n);

    // ---- Head/tail soft fade so beam doesn't end with a flat slice ---------
    float fadeRange = max(HeadTailFade, 0.001);
    float endFade   = smoothstep(0.0, fadeRange,        i.uv.y)
                    * smoothstep(0.0, fadeRange, 1.0 -  i.uv.y);

    // ---- Compose -----------------------------------------------------------
    float3 rgb   = GlowColor.rgb * Intensity * n * rim;
    float  alpha = endFade * rim;

    if (alpha < 0.002) discard;

    // Additive PSO uses src.rgb * src_alpha + dst.rgb * 1 — so multiplying
    // rgb by alpha here gives a stable energy contribution that fades with
    // edges instead of "blowing out + cutting off" abruptly.
    return float4(rgb * alpha, alpha);
}
