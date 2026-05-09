// BeamCommon.hlsli — shared GPU data layouts + parallel-transport helpers
// for the procedural-tube heavy-beam system. Mirrors include/Graphics/BeamSystem.h.

#ifndef BEAM_COMMON_HLSLI
#define BEAM_COMMON_HLSLI

// Tube topology — must match C++ kRadialSegs / kAxialSegs / kMaxControlPointsPerBeam.
#define BEAM_RADIAL_SEGS                 12u
#define BEAM_AXIAL_SEGS                  16u
#define BEAM_RING_VERT_COUNT             (BEAM_RADIAL_SEGS + 1u)   // +1 for UV seam
#define BEAM_AXIAL_RING_COUNT            (BEAM_AXIAL_SEGS + 1u)
#define BEAM_VERTS_PER_BEAM              (BEAM_RING_VERT_COUNT * BEAM_AXIAL_RING_COUNT)
#define BEAM_INDICES_PER_BEAM            (BEAM_RADIAL_SEGS * BEAM_AXIAL_SEGS * 6u)
#define BEAM_MAX_CONTROL_POINTS_PER_BEAM 8u

// 32 bytes — must match C++ BeamControlPointGPU.
struct BeamControlPoint
{
    float3 position;     // 12
    float  radius;       // 4
    float4 colorTint;    // 16  (linear HDR; .a = intensity along beam)
};

// 256 bytes — per-beam compute CBV slot. CS reads this to know where to
// write its slice of the shared VBs and which control-point window to read.
struct BeamGenParams
{
    uint  controlPointOffset;   // 4 — index into gControlPoints[]
    uint  controlPointCount;    // 4 — typically 2..8
    uint  vertexBaseElement;    // 4 — start vertex index in shared VBs
    uint  _pad0;                // 4
    float globalRadiusScale;    // 4 — multiply each control point's radius
    float wobbleAmplitude;      // 4 — perpendicular noise
    float wobbleSpeed;          // 4 — frequency
    float time;                 // 4 — global time for wobble noise
    float4 _pad1;               // 16
    float4 _pad2;               // 16
    float4 _pad3;               // 16
    float4 _pad4;               // 16
    float4 _pad5;               // 16
    float4 _pad6;               // 16
    float4 _pad7;               // 16
    float4 _pad8;               // 16
    float4 _pad9;               // 16
    float4 _pad10;              // 16
    float4 _pad11;              // 16
    float4 _pad12;              // 16
    float4 _pad13;              // 16
    float4 _pad14;              // 16  → 240 bytes used out of 256
};

// Pick a stable reference axis perpendicular to t (for the first-frame seed
// of the parallel-transport frame).
float3 BeamReferenceVector(float3 t)
{
    return abs(t.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
}

#endif // BEAM_COMMON_HLSLI
