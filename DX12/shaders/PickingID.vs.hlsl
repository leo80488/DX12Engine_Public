#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0) { uint meshDescIdx; uint instanceOffset; uint pad0; };
#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);
cbuffer PerViewCB : register(b1, space0) { float4x4 viewProj; };

struct PSIn
{
    float4               sv           : SV_Position;
    nointerpolation uint instanceSlot : TEXCOORD0;
};

PSIn main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];
    uint vid          = FetchIndex(md, rawID);
    float3 localPos   = FETCH_POS(md, vid);

    float4x4 world  = InstanceBuffer[instanceOffset + instID].world;
    float4 worldPos = mul(float4(localPos, 1.0f), world);

    PSIn o;
    o.sv           = mul(worldPos, viewProj);
    o.instanceSlot = instanceOffset + instID;
    return o;
}
