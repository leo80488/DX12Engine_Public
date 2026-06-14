// Grass.ps.hlsl — deferred GBuffer writer for procedural grass blades.
//
// Writes the same 6 MRTs as Terrain.ps / GBuffer.ps so grass participates in
// deferred lighting (CSM shadows, DDGI, SSAO, SSR) with no special cases:
//   RT0 albedo, RT1 normal (encoded *0.5+0.5, a=0), RT2 surface
//   (roughness, metalness, AO, 0.5), RT3 velocity (NDC delta, unjittered),
//   RT4 extra, RT5 HDR emissive seed. Stencil ref 1 (PBR) set by the pass.
//
// Albedo: root→tip gradient × clump/noise tint, root AO darkening (vertex
// data from the MS). Two-sided orientation is owned by the MS (it flips the
// per-vertex normal toward the camera BEFORE the up-blend) — the PS must NOT
// apply a second SV_IsFrontFace flip or back-facing pixels get inverted,
// downward GBuffer normals.

#include "Grass.hlsli"

struct PSIn
{
    float4 sv       : SV_Position;
    float3 wn       : NORMAL;
    float4 col      : COLOR;       // rgb = tint, a = AO along the blade
    float2 misc     : TEXCOORD0;   // x = t along blade
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
};

struct PSOut
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 surface  : SV_TARGET2;
    float2 velocity : SV_TARGET3;
    float4 extra    : SV_TARGET4;
    float4 sceneCol : SV_TARGET5;
};

PSOut main(PSIn i)
{
    PSOut o;

    const float t = saturate(i.misc.x);
    float3 albedo = lerp(g_baseColor.rgb, g_tipColor.rgb, t) * i.col.rgb;

    float3 N = normalize(i.wn);

    o.albedo  = float4(albedo, 1.0);
    o.normal  = float4(N * 0.5 + 0.5, 0.0);
    // surface = (roughness, metalness, AO, 0.5) — matches Terrain.ps encoding.
    o.surface = float4(g_roughness, 0.0, i.col.a, 0.5);

    // Raw NDC delta — matches GBuffer.ps; decoders (TAA/SSR/XeGTAO temporal)
    // apply the (0.5,-0.5) NDC→UV scale themselves.
    float2 cur  = i.curClip.xy  / i.curClip.w;
    float2 prev = i.prevClip.xy / i.prevClip.w;
    o.velocity = cur - prev;

    o.extra    = float4(0, 0, 0, 0);
    o.sceneCol = float4(0, 0, 0, 1);   // no emissive
    return o;
}
