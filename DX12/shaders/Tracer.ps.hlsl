// Tracer.ps.hlsl — additive cylindrical-billboard tracer pixel shader.
//
// Output is HDR pre-multiplied additive (PSO blend = SRC_ALPHA / ONE).
//
// Visual recipe (matches design doc 01_Tracer_*.md §3):
//   finalColor = color.rgb * intensity
//              * coreShape(uv.x)             // triangular cross-section
//              * scrollNoise(uv.y, time)     // along-beam plasma
//              * softParticleFade(depth)     // edge fade against geometry
//              * lifeAlpha                   // age decay
//
// Bindings:
//   b2 space0 — TracerRenderCB (matches Tracer.vs.hlsl)
//   t5 space0 — Texture2D<float> gSceneDepth (hardware depth, reverse-Z)
//   t0 space2 — bindless texture array (optional procedural noise override)
//   s0 space0 — linear-wrap sampler

#include "Tracer.hlsli"
#include "DepthCommon.hlsli"

cbuffer TracerRenderCB : register(b2, space0)
{
    float4x4 viewProj;
    float3   cameraPos;
    float    time;
    float    nearZ;
    float    farZ;
    float    fadeRange;
    float    coreSharpness;
    float    noiseTiling;
    float    scrollSpeed;
    float    noiseFloor;
    float    _pad;
};

Texture2D<float>  gSceneDepth      : register(t5, space0);
Texture2D<float4> g_AllTextures[]  : register(t0, space2);
SamplerState      gLinear          : register(s0, space0);

struct VSOut
{
    float4 pos       : SV_POSITION;
    float2 uv        : TEXCOORD0;
    float4 color     : COLOR0;
    float  lifeAlpha : TEXCOORD1;
    nointerpolation uint noiseIdx : TEXCOORD2;
    float  spawnTime : TEXCOORD3;
};

// Cheap PCG-based hash → unit float for procedural noise.
float Hash12(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

// 1-D smooth value noise along the beam (uv.y axis).
float ProceduralNoise1D(float u, float scrollOffset)
{
    float fp = u + scrollOffset;
    float i  = floor(fp);
    float f  = frac(fp);
    f = f * f * (3.0 - 2.0 * f);   // smoothstep
    float a = Hash12(float2(i,       7.0));
    float b = Hash12(float2(i + 1.0, 7.0));
    return lerp(a, b, f);
}

float4 main(VSOut input) : SV_TARGET
{
    // ---- Core shape: triangular falloff across the beam (uv.x = 0 ... 1) ---
    // Centre is brightest; edges fade. Sharpness power lets art tune the look.
    float core = saturate(1.0 - abs(input.uv.x * 2.0 - 1.0));
    core = pow(core, max(coreSharpness, 0.001));

    // ---- Scrolling noise along beam length ---------------------------------
    // Use spawnTime-relative scroll so each tracer's plasma pattern is stable
    // across frames (no time-dependent wobble that pops on respawn).
    float scrollOff = (time - input.spawnTime) * scrollSpeed;
    float u         = input.uv.y * max(noiseTiling, 0.001);

    float noise;
    if (input.noiseIdx != 0xFFFFFFFFu)
    {
        // Sample a noise texture along (uv.x, scrolled uv.y) for richer pattern.
        float2 nuv = float2(input.uv.x, u - scrollOff);
        noise = g_AllTextures[NonUniformResourceIndex(input.noiseIdx)]
                    .SampleLevel(gLinear, nuv, 0).r;
    }
    else
    {
        noise = ProceduralNoise1D(u, -scrollOff);
    }
    noise = lerp(saturate(noiseFloor), 1.0, noise);

    // ---- Soft-particle fade against scene depth ----------------------------
    int2  px      = int2(input.pos.xy);
    float sceneZ  = gSceneDepth.Load(int3(px, 0));
    float fade    = SoftParticleFade(input.pos.z, sceneZ, nearZ, farZ, fadeRange);

    // ---- Combine -----------------------------------------------------------
    float intensity = max(input.color.a, 0.0);
    float3 rgb      = input.color.rgb * intensity;
    float  alpha    = core * noise * fade * input.lifeAlpha;

    // Drop subpixel-dim contributions to skip blend hardware work.
    if (alpha < 0.002) discard;

    return float4(rgb, alpha);
}
