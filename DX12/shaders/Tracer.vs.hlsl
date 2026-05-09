// Tracer.vs.hlsl — cylindrical-billboard quad VS driven by StructuredBuffer<Tracer>.
//
// One instance per pool slot; 4 verts per instance (TRIANGLESTRIP). Dead
// tracers (lifetime <= 0) emit a degenerate quad — rasteriser discards the
// zero-area triangles without entering PS. Same dead-particle pattern as
// Particle.vs.hlsl.
//
// Cylindrical billboard math (matches design doc 01_Tracer_*.md §2.3):
//   sideDir = normalize(cross(beamAxis, toCamera))
//   With degeneracy fallback when beam is nearly parallel to view ray.
//
// Root signature (graphics, space0):
//   b2 space0 → TracerRenderCB (viewProj, cameraPos, time, fadeRange...)
//   t4 space0 → DESC_TABLE → StructuredBuffer<Tracer> gPool

#include "Tracer.hlsli"

cbuffer TracerRenderCB : register(b2, space0)
{
    float4x4 viewProj;       // jittered (for SV_POSITION)
    float3   cameraPos;
    float    time;
    float    nearZ;
    float    farZ;
    float    fadeRange;      // soft-particle fade distance, view-space metres
    float    coreSharpness;  // PS triangular falloff exponent
    float    noiseTiling;    // along-beam noise tile count
    float    scrollSpeed;    // along-beam noise UV/sec
    float    noiseFloor;     // 0..1 minimum noise multiplier
    float    _pad;
};

StructuredBuffer<Tracer> gPool : register(t4, space0);

struct VSOut
{
    float4 pos       : SV_POSITION;
    float2 uv        : TEXCOORD0;     // x = side (0..1), y = along-beam (0..1)
    float4 color     : COLOR0;
    float  lifeAlpha : TEXCOORD1;     // 0..1, fades to 0 over life
    nointerpolation uint noiseIdx : TEXCOORD2;  // bindless idx, 0xFFFFFFFFu = procedural
    float  spawnTime : TEXCOORD3;     // for noise scroll continuity across frames
};

// 4 verts per instance: triangle-strip quad along beam axis.
//   v0 = (-side, start),  v1 = (+side, start)
//   v2 = (-side, end),    v3 = (+side, end)
// quadOffsets.x = side (-0.5..0.5), .y = along-beam (0..1).
static const float2 kQuadOffsets[4] =
{
    float2(-0.5, 0.0),
    float2( 0.5, 0.0),
    float2(-0.5, 1.0),
    float2( 0.5, 1.0),
};

VSOut main(uint vID : SV_VertexID, uint iID : SV_InstanceID)
{
    VSOut o = (VSOut)0;

    Tracer t = gPool[iID];

    if (t.lifetime <= 0.0 || t.maxLifetime <= 0.0)
    {
        // Degenerate quad — rasteriser discards.
        o.pos       = float4(0, 0, 0, 1);
        o.color     = float4(0, 0, 0, 0);
        o.noiseIdx  = 0xFFFFFFFFu;
        return o;
    }

    const float2 q = kQuadOffsets[vID];

    // Position along beam centerline at this vertex's UV.y parameter.
    float3 axisPos = lerp(t.startPos, t.endPos, q.y);

    float3 beamSeg = t.endPos - t.startPos;
    float  beamLen = length(beamSeg);
    float3 beamAxis = beamLen > 1e-4 ? beamSeg / beamLen : float3(0, 1, 0);

    float3 toCam = cameraPos - axisPos;
    float  toCamLen = length(toCam);
    float3 toCamN = toCamLen > 1e-4 ? toCam / toCamLen : float3(0, 0, 1);

    // Cylindrical billboard: sideDir is perpendicular to both beam and view.
    float3 sideDir = cross(beamAxis, toCamN);
    float  sideLen = length(sideDir);

    // Degenerate when view ray is nearly parallel to beam axis.
    if (sideLen < 1e-3)
    {
        // Pick any axis perpendicular to beamAxis as a stable fallback.
        float3 up = abs(beamAxis.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
        sideDir = cross(beamAxis, up);
        sideLen = length(sideDir);
    }
    sideDir = sideLen > 1e-4 ? sideDir / sideLen : float3(1, 0, 0);

    // Expand quad sideways by world-space width.
    float3 worldPos = axisPos + sideDir * (q.x * t.width);

    // Optional sub-pixel widening at distance to keep thin tracers from
    // aliasing into nothing on far-away shots. Compensated by alpha attenuation
    // in PS to keep total energy roughly constant.
    o.pos = mul(float4(worldPos, 1.0), viewProj);

    // UV: x = 0..1 across the quad; y = 0..1 along the beam.
    o.uv = float2(q.x + 0.5, q.y);
    o.color     = t.color;
    o.lifeAlpha = saturate(t.lifetime / t.maxLifetime);
    o.noiseIdx  = t.noiseTexBindlessIdx;
    o.spawnTime = t.spawnTime;

    return o;
}
