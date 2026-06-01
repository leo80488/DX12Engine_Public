#ifndef GPU_INSTANCE_HLSLI
#define GPU_INSTANCE_HLSLI

// gpu_instance.hlsli — GPU instance data layout.
// Mirrors C++ GPUInstanceData in include/Graphics/GPUInstanceData.h.

struct GPUInstanceData
{
    float4x4 world;          // 64 bytes — transposed current world matrix
    uint     meshDescIdx;    //  4 bytes
    uint     materialIdx;    //  4 bytes
    uint     lodLevel;       //  4 bytes
    uint     pad;            //  4 bytes
    float4x4 prevWorld;      // 64 bytes — transposed prev-frame world (TAA velocity)
};                            // (total: 144 bytes)

#endif // GPU_INSTANCE_HLSLI
