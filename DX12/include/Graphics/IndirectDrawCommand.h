#pragma once

// IndirectDrawCommand — matches the command signature for ExecuteIndirect.
//
// Layout:
//   [0-3] Root constants (4 x uint32 at b0 space0):
//         meshDescIdx, instanceOffset, materialIndex, prevPosInfo
//   [4-7] D3D12_DRAW_ARGUMENTS:
//         VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation
//
// Total: 8 x uint32 = 32 bytes per command.

#include <cstdint>

struct IndirectDrawCommand
{
    // Root constants (must match root param 0 layout)
    uint32_t meshDescIdx;
    uint32_t instanceOffset;
    uint32_t materialIndex;
    uint32_t prevPosInfo;

    // D3D12_DRAW_ARGUMENTS
    uint32_t vertexCountPerInstance;
    uint32_t instanceCount;
    uint32_t startVertexLocation;
    uint32_t startInstanceLocation;
};

static_assert(sizeof(IndirectDrawCommand) == 32, "IndirectDrawCommand must be 32 bytes");
