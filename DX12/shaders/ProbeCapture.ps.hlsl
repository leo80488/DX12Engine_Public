// ProbeCapture.ps.hlsl — minimal lit forward pixel shader for reflection
// probe baking. Pairs with GBuffer.vs.hlsl unchanged. Reads sun direction +
// colour from a per-face capture CB at b1 (which also holds the face viewProj
// the GBuffer VS consumes). Writes ONE HDR color RTV — feeds the prefilter
// compute shader downstream.
//
// Intentionally simple:
//   * Lambert direct sun
//   * SkyIBL SH ambient (read at t19 space0, populated by SkyIBLPass)
//   * No clustered point/spot lights
//   * No shadow maps (probes capture pre-shadow scene; shadow pass would
//     duplicate the scene draw cost per probe)
//   * No normal map (V1 — can be added later, requires sampling the bindless
//     normal slot and TBN math identical to GBuffer.ps)
//
// Output is HDR linear so the GGX prefilter dispatch can mip-average it
// without colour bleed from a tonemapped intermediate.

#include "material.hlsli"

// Push constants — same b0 the VS uses (only materialIndex matters here).
cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint prevPosInfo;
};

// The capture CB shares register b1 with PerViewCB. The first 3 matrices
// match GBuffer.vs.hlsl exactly (so the VS works unchanged); the trailing
// scalars are capture-specific and the VS just ignores them.
cbuffer ProbeCaptureCB : register(b1, space0)
{
    float4x4 c_viewProj;
    float4x4 c_prevViewProj;
    float4x4 c_curViewProjNoJitter;
    float3   c_sunDir;     float c_pad0;
    float3   c_sunColor;   float c_pad1;
    float3   c_cameraPos;  float c_pad3;
};

StructuredBuffer<MaterialGPUData> g_Materials : register(t2, space0);

// Bindless texture pool — same convention as GBuffer.ps.
Texture2D g_AllTextures[] : register(t0, space2);

// Per-draw fallback base color + normal slots when bindless isn't valid.
Texture2D<float4> g_BaseColor : register(t3, space0);
Texture2D<float4> g_NormalMap : register(t5, space0);

// Sky SH coefficient buffer — populated by SkyIBLPass, sampled here for the
// ambient term. Indexed identically to Lighting.ps's EvalSH2.
StructuredBuffer<float4> gSkySH : register(t19, space0);

SamplerState g_LinearWrap : register(s0, space0);

// Mirror of Lighting.ps's EvalSH2 — kept inline so this shader is
// self-contained (the lighting shader's helper sits behind a mountain of
// includes we don't want to drag into the capture pipeline).
float3 EvalSH2(float3 N)
{
    const float b0 = 0.282095;
    const float b1 = 0.488603 * N.y;
    const float b2 = 0.488603 * N.z;
    const float b3 = 0.488603 * N.x;
    const float b4 = 1.092548 * N.x * N.y;
    const float b5 = 1.092548 * N.y * N.z;
    const float b6 = 0.315392 * (3.0 * N.z * N.z - 1.0);
    const float b7 = 1.092548 * N.z * N.x;
    const float b8 = 0.546274 * (N.x * N.x - N.y * N.y);

    return max(0.0,
        gSkySH[0].rgb * b0 +
        gSkySH[1].rgb * b1 +
        gSkySH[2].rgb * b2 +
        gSkySH[3].rgb * b3 +
        gSkySH[4].rgb * b4 +
        gSkySH[5].rgb * b5 +
        gSkySH[6].rgb * b6 +
        gSkySH[7].rgb * b7 +
        gSkySH[8].rgb * b8);
}

struct PSIn
{
    float4 sv       : SV_POSITION;
    float3 worldPos : POSITIONWS;
    float2 uv       : TEXCOORD0;
    float3 wn       : NORMAL;
    float3 wt       : TANGENT;
    float3 wbt      : BINORMAL;
    float3 col      : COLOR;
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
};

float4 main(PSIn i) : SV_TARGET
{
    MaterialGPUData mat = g_Materials[materialIndex];

    float4 baseColor = mat.baseColor;
    if (mat.paramCount == 0)
        baseColor = float4(i.col, 1.0);  // pre-material-system fallback

    int texBaseColor = mat.textureHandleIds[0];
    if (texBaseColor >= 0)
        baseColor *= g_AllTextures[texBaseColor].Sample(g_LinearWrap, i.uv);
    else
        baseColor *= g_BaseColor.Sample(g_LinearWrap, i.uv);

    // Normal mapping (mirrors GBuffer.ps): unpack tangent-space RG, derive Z,
    // optionally scale by the material's normal-strength knob, then transform
    // through the interpolated TBN to world space. Falls back to the geometry
    // normal if the material has no normal map (texNormal < 0).
    int    texNormal      = mat.textureHandleIds[1];
    float  normalStrength = mat.normalStrength;
    if (mat.paramCount == 0) normalStrength = 1.0;

    float2 rg = (texNormal >= 0)
                ? g_AllTextures[texNormal].Sample(g_LinearWrap, i.uv).rg * 2.0 - 1.0
                : g_NormalMap.Sample(g_LinearWrap, i.uv).rg * 2.0 - 1.0;
    float  nz = sqrt(saturate(1.0 - dot(rg, rg)));
    float3 ts = float3(rg, nz);
    ts.xy *= normalStrength;
    ts = normalize(ts);

    float3 N  = normalize(i.wn);
    float3 T  = normalize(i.wt);
    float3 BT = normalize(i.wbt);
    float3x3 TBN = float3x3(T, BT, N);
    N = normalize(mul(ts, TBN));

    // Direct sun — Lambert. Sun direction in capture CB is the world-space
    // direction the light TRAVELS in, so the surface-toward-sun vector flips.
    float3 L    = normalize(-c_sunDir);
    float  NdL  = saturate(dot(N, L));
    float3 sun  = baseColor.rgb * c_sunColor * NdL;

    // Ambient — SkySH gives a low-frequency environment irradiance that
    // matches what the lighting pass uses for the diffuse IBL term, so
    // baked probes inherit the same colour palette as the final shading.
    float3 amb  = baseColor.rgb * EvalSH2(N);

    // Output HDR linear. No tonemap, no exposure — the prefilter compute
    // shader needs raw radiance to generate physically-meaningful mip levels.
    return float4(sun + amb, 1.0);
}
