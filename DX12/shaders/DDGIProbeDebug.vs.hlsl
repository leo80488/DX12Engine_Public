// DDGIProbeDebug.vs.hlsl — emits one procedural UV sphere per probe.
//
// 16 rings × 16 sectors = 256 quads × 6 verts/quad (unindexed tri-list) =
// 1536 verts per probe instance. The sphere geometry is computed entirely
// from SV_VertexID + spherical-coordinate trig; no VB/IB binding needed —
// the host code just calls DrawInstanced(1536, probeCount, 0, 0).
//
// Per-instance: SV_InstanceID is the probe's linear index into the volume's
// grid. The VS resolves the world-space probe centre via DDGI_ProbeWorldPos
// and offsets each vertex by `nObj * sphereRadius` so the geometry sits as
// a unit sphere centred on that probe.

#include "DDGICommon.hlsli"

cbuffer PerViewCB : register(b0, space0)
{
    float4x4 g_ViewProj;        // current viewProj — ignore the rest
    float4x4 _pad0;
    float4x4 _pad1;
};

ConstantBuffer<DDGIVolumeGPU> g_Vol : register(b1, space0);

// ProbeData SRV — VS uses `pd.offset` so the visualization matches the
// relocation-adjusted positions trace + sampling actually use.
StructuredBuffer<DDGIProbeData> g_ProbeData : register(t1, space0);

cbuffer DebugConstants : register(b2, space0)
{
    float g_SphereRadius;
    float _padR0, _padR1, _padR2;
};

struct VSOut
{
    float4 pos      : SV_POSITION;
    float3 normal   : NORMAL;
    uint   probeIdx : PROBE_IDX;
};

// Sphere tessellation — keep low enough that a 16³ grid (4096 probes) is
// affordable: 16² × 6 × 4096 = ~6.3M verts per frame. Bumping to 32² is fine
// for typical 256-probe grids if the user wants smoother spheres.
static const uint kSegments = 16;

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    // Probe coord from instance index. World position includes the
    // relocation offset so the debug sphere sits at the same place the trace
    // origin and DDGI_SampleVolume use.
    int3 coord = DDGI_ProbeCoord(iid, g_Vol);
    float3 probeWorld = DDGI_ProbeWorldPos(coord, g_Vol);
    {
        DDGIProbeData pd = g_ProbeData[iid];
        probeWorld += pd.offset;
    }

    // Decompose vid into (quad, corner) → (ring, sector, corner).
    uint quadId   = vid / 6u;
    uint cornerId = vid % 6u;
    uint ringId   = quadId / kSegments;
    uint secId    = quadId % kSegments;

    // 6-vertex triangle pair: corners 0,1,2 and 0,2,3 share a quad.
    // Quad corner indices (CCW when viewed from outside):
    //   0 → (ring,   sec  )
    //   1 → (ring+1, sec  )
    //   2 → (ring+1, sec+1)
    //   3 → (ring,   sec+1)
    static const uint kQuadCorner[6] = { 0, 1, 2, 0, 2, 3 };
    uint c = kQuadCorner[cornerId];
    uint dRing = (c == 1 || c == 2) ? 1u : 0u;
    uint dSec  = (c == 2 || c == 3) ? 1u : 0u;

    uint ring = ringId + dRing;
    uint sec  = secId  + dSec;

    // Spherical coordinates → unit sphere normal/position.
    float theta = 3.14159265 * float(ring) / float(kSegments);            // 0..π
    float phi   = 6.28318530 * float(sec)  / float(kSegments);            // 0..2π
    float sinT  = sin(theta), cosT = cos(theta);
    float sinP  = sin(phi),   cosP = cos(phi);
    float3 nObj = float3(sinT * cosP, cosT, sinT * sinP);

    float3 worldPos = probeWorld + nObj * g_SphereRadius;

    VSOut o;
    o.pos      = mul(float4(worldPos, 1.0), g_ViewProj);
    o.normal   = nObj;
    o.probeIdx = iid;
    return o;
}
