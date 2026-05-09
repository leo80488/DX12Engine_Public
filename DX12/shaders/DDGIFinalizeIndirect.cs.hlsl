// DDGIFinalizeIndirect.cs.hlsl — write the trace dispatch's indirect args.
//
// Reads g_RayAlloc[0] (total rays packed by DDGIRayAllocation.cs) and writes
// the dispatch group dimensions into g_DispatchArgs[0..2]. The args buffer is
// physically separate so it can transition to D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
// for the ExecuteIndirect call without conflicting with the descriptor
// buffer's UAV state.

#include "DDGICommon.hlsli"

RWStructuredBuffer<uint> g_RayAlloc     : register(u3, space0);
RWStructuredBuffer<uint> g_DispatchArgs : register(u4, space0);

[numthreads(1, 1, 1)]
void main()
{
    const uint total   = g_RayAlloc[0];
    const uint groupsX = (total + 31u) / 32u;
    g_DispatchArgs[0] = max(groupsX, 1u);
    g_DispatchArgs[1] = 1u;
    g_DispatchArgs[2] = 1u;
}
