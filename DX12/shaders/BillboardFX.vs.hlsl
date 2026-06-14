// BillboardFX.vs.hlsl — animated sprite-sheet billboard quads.
//
// The CPU (BillboardFXPass) pre-expands a camera-facing quad per effect and
// bakes the current flipbook frame's atlas sub-rect straight into the vertex
// UVs, so the VS only transforms world → clip and forwards uv / HDR colour /
// bindless texIdx.  Colour is a FLOAT4 (not packed RGBA8) so emissive > 1 can
// reach the HDR target and bloom.
//
// Vertex layout (40 bytes, vertex-pull from ByteAddressBuffer at t2 space0):
//   [0..11]  float3  worldPos
//   [12..19] float2  uv          (atlas sub-rect, baked per-frame on CPU)
//   [20..35] float4  colour      (tint.rgb*emissive, tint.a*opacity — HDR)
//   [36..39] uint32  bindless texIdx (0xFFFFFFFF = untextured, colour only)

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 g_viewProj;
};

ByteAddressBuffer g_VB : register(t2, space0);

struct VSOut
{
    float4 pos                  : SV_POSITION;
    float2 uv                   : TEXCOORD0;
    nointerpolation uint texIdx : TEXCOORD1;
    float4 color                : COLOR0;
};

VSOut main(uint vid : SV_VertexID)
{
    const uint stride = 40u;
    const uint off = vid * stride;

    float3 wp;
    wp.x = asfloat(g_VB.Load(off + 0u));
    wp.y = asfloat(g_VB.Load(off + 4u));
    wp.z = asfloat(g_VB.Load(off + 8u));

    float2 uv;
    uv.x = asfloat(g_VB.Load(off + 12u));
    uv.y = asfloat(g_VB.Load(off + 16u));

    float4 col;
    col.r = asfloat(g_VB.Load(off + 20u));
    col.g = asfloat(g_VB.Load(off + 24u));
    col.b = asfloat(g_VB.Load(off + 28u));
    col.a = asfloat(g_VB.Load(off + 32u));

    uint texIdx = g_VB.Load(off + 36u);

    VSOut o;
    o.pos    = mul(float4(wp, 1.0), g_viewProj);
    o.uv     = uv;
    o.texIdx = texIdx;
    o.color  = col;
    return o;
}
