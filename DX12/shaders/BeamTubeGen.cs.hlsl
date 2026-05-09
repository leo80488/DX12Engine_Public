// BeamTubeGen.cs.hlsl — generate one beam tube's vertex data from CPU-uploaded
// control points using a Parallel Transport Frame.
//
// One CS dispatch per beam. ThreadGroupCount = ceil(BEAM_AXIAL_RING_COUNT / 8).
// Each thread (axisIdx) builds one ring of BEAM_RING_VERT_COUNT vertices and
// writes them into the shared PVF buffers at vertexBaseElement + axisIdx * ring.
//
// Algorithm (matches design doc 02_HeavyBeam_*.md §2.4):
//   1. Resample the input control points to BEAM_AXIAL_RING_COUNT positions
//      (linear interp; with N=2 control points this is just lerp(start,end)).
//   2. Walk the resampled points, building a parallel-transport frame —
//      at each step, rotate the previous (right, up) basis by the rotation
//      that takes the previous tangent into the current tangent.
//   3. For each axisIdx, expand a ring of BEAM_RING_VERT_COUNT verts around
//      the tangent using right * cos(theta) + up * sin(theta) * radius.
//   4. Write pos / normal (= unit offset from centerline) / tangent (= along beam)
//      / uv (.x = around ring, .y = along beam) into the shared UAV buffers.
//
// Note: a single thread per axial ring keeps the parallel-transport state
// trivially serial — no need for prefix-scan or shared memory. With axisCount
// = 17 typical, the dispatch underuses the wave but we don't care: this
// runs once per beam per frame.
//
// Root signature (compute, mirrors ParticleSimPass slots):
//   b0 space2 — BeamGenParams CBV (per-beam)
//   t0 space2 — StructuredBuffer<BeamControlPoint> gControlPoints
//   u0 space2 — RWByteAddressBuffer gPositions   (12B/vert, VF_FLOAT3)
//   u1 space2 — RWByteAddressBuffer gNormals     (12B/vert, VF_FLOAT3)
//   u2 space2 — RWByteAddressBuffer gTangents    (16B/vert, VF_FLOAT4 — w = 1)
//   u3 space2 — RWByteAddressBuffer gUVs         ( 8B/vert, VF_FLOAT2)

#include "BeamCommon.hlsli"

cbuffer ParamsCB : register(b0, space2)
{
    BeamGenParams gParams;
};

StructuredBuffer<BeamControlPoint> gControlPoints : register(t0, space2);

RWByteAddressBuffer gPositions : register(u0, space2);
RWByteAddressBuffer gNormals   : register(u1, space2);
RWByteAddressBuffer gTangents  : register(u2, space2);
RWByteAddressBuffer gUVs       : register(u3, space2);

// Hash-based 3D noise for the perpendicular wobble.
float Hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

// Resample control points uniformly along the polyline. With N=2 (start+end)
// and `t` in 0..1, linearly interpolate. With N>2, treat the points as
// equally spaced (no arc-length parameterisation — good enough for short
// beams; revisit if curvature varies wildly between segments).
//   tNorm = axisIdx / BEAM_AXIAL_SEGS   (0 = start ring, 1 = end ring)
void ResampleControlPoint(float tNorm, out float3 pos, out float radius, out float4 tint)
{
    const uint cpCount = max(gParams.controlPointCount, 2u);
    const float scaledT = tNorm * float(cpCount - 1u);
    const uint  i0 = min(uint(floor(scaledT)), cpCount - 2u);
    const uint  i1 = i0 + 1u;
    const float frac_ = scaledT - float(i0);

    BeamControlPoint a = gControlPoints[gParams.controlPointOffset + i0];
    BeamControlPoint b = gControlPoints[gParams.controlPointOffset + i1];

    pos    = lerp(a.position,  b.position,  frac_);
    radius = lerp(a.radius,    b.radius,    frac_) * gParams.globalRadiusScale;
    tint   = lerp(a.colorTint, b.colorTint, frac_);
}

// Compute the tangent at axisIdx by central difference (or one-sided at ends).
float3 TangentAt(uint axisIdx)
{
    const float dt = 1.0 / float(BEAM_AXIAL_SEGS);
    const uint  iPrev = (axisIdx == 0u) ? 0u : axisIdx - 1u;
    const uint  iNext = (axisIdx == BEAM_AXIAL_SEGS) ? BEAM_AXIAL_SEGS : axisIdx + 1u;

    float3 pPrev, pNext; float rDummy; float4 tintDummy;
    ResampleControlPoint(float(iPrev) * dt, pPrev, rDummy, tintDummy);
    ResampleControlPoint(float(iNext) * dt, pNext, rDummy, tintDummy);

    const float3 d = pNext - pPrev;
    const float  len = length(d);
    return len > 1e-5 ? d / len : float3(0, 1, 0);
}

// Rotate vector v around (unit) axis by sin/cos. Rodrigues' formula.
float3 RotateRodrigues(float3 v, float3 axis, float sinT, float cosT)
{
    return v * cosT + cross(axis, v) * sinT + axis * dot(axis, v) * (1.0 - cosT);
}

[numthreads(8, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint axisIdx = id.x;
    if (axisIdx >= BEAM_AXIAL_RING_COUNT) return;

    // ---- Walk parallel-transport frame from axisIdx 0 .. axisIdx ----------
    // Each thread independently re-walks from the start; this is wasteful
    // (O(BEAM_AXIAL_RING_COUNT^2) work total) but a) keeps the kernel
    // dependency-free, and b) BEAM_AXIAL_RING_COUNT is tiny (17). For
    // larger tubes consider a serial single-thread pass + group-shared
    // store.
    float3 t0       = TangentAt(0u);
    float3 right0   = normalize(cross(t0, BeamReferenceVector(t0)));
    float3 up0      = cross(right0, t0);
    float3 right    = right0;
    float3 up       = up0;
    float3 tPrev    = t0;

    for (uint k = 1u; k <= axisIdx; ++k)
    {
        float3 tNext = TangentAt(k);
        float3 axis = cross(tPrev, tNext);
        float  axisLen = length(axis);
        if (axisLen > 1e-5)
        {
            axis = axis / axisLen;
            float cosT = clamp(dot(tPrev, tNext), -1.0, 1.0);
            float sinT = axisLen; // |a × b| = sin(angle) for unit a,b
            right = normalize(RotateRodrigues(right, axis, sinT, cosT));
            up    = normalize(RotateRodrigues(up,    axis, sinT, cosT));
        }
        tPrev = tNext;
    }

    // ---- Sample centerline at this ring ------------------------------------
    const float dt = 1.0 / float(BEAM_AXIAL_SEGS);
    const float tNorm = float(axisIdx) * dt;
    float3 centerPos; float radius; float4 tint;
    ResampleControlPoint(tNorm, centerPos, radius, tint);

    // Optional wobble: perpendicular noise at frequency wobbleSpeed.
    if (gParams.wobbleAmplitude > 0.0)
    {
        float3 nseed = centerPos * 0.5 + float3(0, gParams.time * gParams.wobbleSpeed, 0);
        float  nx = Hash13(nseed) * 2.0 - 1.0;
        float  ny = Hash13(nseed.zxy + 17.0) * 2.0 - 1.0;
        centerPos += (right * nx + up * ny) * gParams.wobbleAmplitude;
    }

    // ---- Write the ring (BEAM_RING_VERT_COUNT verts, +1 for UV seam) ------
    const uint ringStart = gParams.vertexBaseElement + axisIdx * BEAM_RING_VERT_COUNT;
    const float invRadial = 1.0 / float(BEAM_RADIAL_SEGS);

    for (uint i = 0u; i < BEAM_RING_VERT_COUNT; ++i)
    {
        const float u = float(i) * invRadial;
        const float ang = u * 6.28318530718;
        const float c = cos(ang);
        const float s = sin(ang);

        float3 outward = right * c + up * s;
        float3 vPos    = centerPos + outward * radius;
        float3 vNrm    = outward;            // tube normal points radially out
        float4 vTan    = float4(tPrev, 1.0); // tangent = beam axis at this ring
        float2 vUV     = float2(u, tNorm);   // .x along ring, .y along beam

        const uint vert = ringStart + i;
        gPositions.Store3(vert * 12u, asuint(vPos));
        gNormals  .Store3(vert * 12u, asuint(vNrm));
        gTangents .Store4(vert * 16u, asuint(vTan));
        gUVs      .Store2(vert *  8u, asuint(vUV));
    }
}
