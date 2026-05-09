// Trail.vs.hlsl — camera-facing ribbon from consecutive segment pairs.
//
// Instancing layout:
//   InstanceID encodes (trailIdx, segPairIdx):
//     trailIdx    = InstanceID / (maxSegments - 1)
//     segPairIdx  = InstanceID % (maxSegments - 1)
//   VertexID (0..5) = one of the 6 verts of the 2-triangle quad between
//   segmentA (older) and segmentB (newer).
//
// Dead instances (segPairIdx >= header.count - 1, or header.count < 2) emit
// a degenerate quad so the rasteriser drops them without a separate cull.
//
// Root signature (graphics space0):
//   b2 space0 → TrailRenderCB (viewProj + camForward + maxSegments)
//   t4 space0 → StructuredBuffer<TrailSegment> gSegments
//   t5 space0 → StructuredBuffer<TrailHeader>  gHeaders

#include "Trail.hlsli"

cbuffer TrailRenderCB : register(b2, space0)
{
    float4x4 viewProj;
    float3   camForward;    // used to flatten ribbon toward camera
    uint     maxSegments;
};

StructuredBuffer<TrailSegment> gSegments : register(t4, space0);
StructuredBuffer<TrailHeader>  gHeaders  : register(t5, space0);

struct VSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float  alpha : TEXCOORD0;  // additional per-vertex fade
};

static const float2 kQuadUV[6] =
{
    float2(0, 0), // v0: segA - perp
    float2(1, 0), // v1: segB - perp  ← actually v2 in quad sense
    float2(0, 1), // v2: segA + perp
    float2(0, 1), // v3 repeat
    float2(1, 0), // v4 repeat
    float2(1, 1), // v5: segB + perp
};

// Per-vert choice: (which segment) × (+ or - perpendicular)
//   vertexID 0 = (A, -perp)
//   vertexID 1 = (B, -perp)
//   vertexID 2 = (A, +perp)
//   vertexID 3 = (A, +perp)
//   vertexID 4 = (B, -perp)
//   vertexID 5 = (B, +perp)
VSOut main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOut o = (VSOut)0;

    const uint segsPerTrail = maxSegments;
    const uint pairsPerTrail = segsPerTrail - 1;
    const uint trailIdx    = instanceID / pairsPerTrail;
    const uint segPairIdx  = instanceID % pairsPerTrail;

    TrailHeader hdr = gHeaders[trailIdx];

    // Not enough history to form a ribbon pair.
    if (hdr.count < 2u || segPairIdx >= hdr.count - 1u)
    {
        o.pos = float4(0, 0, 0, 1);
        return o;
    }

    // Logical (oldest-first) → physical ring slot.
    // Oldest logical index 0 lives at physical = (head + maxSegs - count) % maxSegs
    const uint oldestPhys = (hdr.head + segsPerTrail - hdr.count) % segsPerTrail;
    const uint physA = (oldestPhys + segPairIdx)     % segsPerTrail;
    const uint physB = (oldestPhys + segPairIdx + 1) % segsPerTrail;

    TrailSegment sA = gSegments[trailIdx * segsPerTrail + physA];
    TrailSegment sB = gSegments[trailIdx * segsPerTrail + physB];

    // Dead segment pair (either past maxAge) → degenerate.
    if (sA.age > hdr.maxAge && sB.age > hdr.maxAge)
    {
        o.pos = float4(0, 0, 0, 1);
        return o;
    }

    // Choose the target segment + side for this vertex.
    // v0/v2/v3 sit on segA; v1/v4/v5 sit on segB.
    // +perp for v2/v3/v5; -perp for v0/v1/v4.
    const bool onB   = (vertexID == 1u) || (vertexID == 4u) || (vertexID == 5u);
    const bool plus  = (vertexID == 2u) || (vertexID == 3u) || (vertexID == 5u);

    // Resolve the target segment's fields via scalar selects — HLSL ternary
    // on struct types is rejected as "type mismatch between conditional
    // values" on some compilers.
    float3 segPos = onB ? sB.position : sA.position;
    float  segAge = onB ? sB.age      : sA.age;

    // Ribbon tangent = A→B. Perpendicular = cross(tangent, camForward).
    // Works because camForward is ~normal to the ribbon's screen plane, so
    // (tangent × camForward) lies roughly in screen space and perpendicular
    // to the tangent — exactly the extrusion axis we want.
    float3 tangent = sB.position - sA.position;
    float3 perp    = cross(normalize(tangent + 1e-6), normalize(camForward));
    perp           = normalize(perp + 1e-6);

    // Narrow the ribbon as the segment ages (older = thinner).
    const float ageT  = saturate(segAge / max(hdr.maxAge, 1e-4));
    const float halfW = hdr.width * (1.0 - 0.6 * ageT);

    float3 worldPos = segPos + perp * (plus ? halfW : -halfW);
    o.pos = mul(float4(worldPos, 1.0), viewProj);

    // Age-based color + alpha fade.
    o.color = lerp(hdr.startColor, hdr.endColor, ageT);
    o.alpha = 1.0 - ageT;

    return o;
}
