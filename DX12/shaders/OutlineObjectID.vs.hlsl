// OutlineObjectID.vs.hlsl — minimal PVF VS for the outline ObjectID sub-pass.
//
// Same projection path as InvertedHull.vs.hlsl: uses curViewProjNoJitter so
// the ObjectID texture is rasterized at the SAME un-jittered NDC as the
// TAA-resolved scene + InvertedHull outline. Without this, ObjectID lives at
// jittered NDC and screen-space inner edges drift ~0.5 px off the mesh edge
// in the resolved LDR — particularly visible during animation when TAA's
// velocity reprojection pulls the displayed mesh to un-jittered positions.

#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor>  MeshDescriptors : register(t1, space0);

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;             // JITTERED — NOT used here
    float4x4 prevViewProj;         // unused
    float4x4 curViewProjNoJitter;  // un-jittered — used for SV_POSITION
};

struct VSOut { float4 sv : SV_POSITION; };

VSOut main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md  = MeshDescriptors[meshDescIdx];
    uint           vid = FetchIndex(md, rawID);

    float3   localPos = FETCH_POS(md, vid);
    float4x4 world    = InstanceBuffer[instanceOffset + instID].world;

    float4 wPos    = mul(float4(localPos, 1.0f), world);
    float4 clipPos = mul(wPos, curViewProjNoJitter);

    VSOut o;
    o.sv = clipPos;
    return o;
}
