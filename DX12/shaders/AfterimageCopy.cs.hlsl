// AfterimageCopy.cs.hlsl — copy a slice of the post-skinning vertex buffer
// (SkinnedVertexRing pos/nrm) into a long-lived afterimage snapshot pool.
//
// One CS dispatch per snapshot, dispatchX = ceil(vertexCount / 64).
// Per-dispatch parameters arrive via the same root CBV slot SkinningPass
// already uses (b0, space2), with descriptor tables for the source SRVs
// and destination UAVs.
//
// Layout matches what AfterimageCapturePass binds:
//   [0] ROOT_CBV    b0 space2 — AfterimageCopyJob
//   [1] DESC_TABLE  t0 space2 — SkinnedVertexRing pos SRV   (StructuredBuffer<float3>)
//   [2] DESC_TABLE  t1 space2 — SkinnedVertexRing nrm SRV   (StructuredBuffer<float3>)
//   [4] DESC_TABLE  u0 space2 — Afterimage pool pos UAV     (RWStructuredBuffer<float3>)
//   [5] DESC_TABLE  u1 space2 — Afterimage pool nrm UAV     (RWStructuredBuffer<float3>)
//
// Source / destination strides are both 12 B/vertex (float3) — same as
// SkinnedVertexRing. *ElementBase indices are pre-divided by 12 on the CPU.

struct AfterimageCopyJob
{
    uint  srcPosElementBase;
    uint  srcNrmElementBase;
    uint  dstPosElementBase;
    uint  dstNrmElementBase;
    uint  vertexCount;
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

ConstantBuffer<AfterimageCopyJob> g_Job : register(b0, space2);

StructuredBuffer<float3>   g_SrcPos : register(t0, space2);
StructuredBuffer<float3>   g_SrcNrm : register(t1, space2);

RWStructuredBuffer<float3> g_DstPos : register(u0, space2);
RWStructuredBuffer<float3> g_DstNrm : register(u1, space2);

[numthreads(64, 1, 1)]
void AfterimageCopyCS(uint3 tid : SV_DispatchThreadID)
{
    const uint v = tid.x;
    if (v >= g_Job.vertexCount) return;

    g_DstPos[g_Job.dstPosElementBase + v] = g_SrcPos[g_Job.srcPosElementBase + v];
    g_DstNrm[g_Job.dstNrmElementBase + v] = g_SrcNrm[g_Job.srcNrmElementBase + v];
}
