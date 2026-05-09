// Shadow.vs.hlsl — depth-only vertex shader for cascade shadow map rendering.
//
// Reads vertex position via PVF (same ByteAddressBuffer layout as GBuffer.vs.hlsl).
// Uses a per-cascade shadow view-projection matrix bound at b1 space0 — the same
// root slot as PerViewCB in GBuffer.vs.hlsl, but with a different buffer bound by
// ShadowPass per cascade.

#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);

cbuffer ShadowPerViewCB : register(b1, space0)
{
    float4x4 shadowViewProj;
};

struct VSOut
{
    float4 sv : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];
    uint vid = FetchIndex(md, rawID);

    float3   localPos = FETCH_POS(md, vid);
    float2   uv0      = FETCH_UV0(md, vid);
    float4x4 world    = InstanceBuffer[instanceOffset + instID].world;
    float4   wPos     = mul(float4(localPos, 1.0f), world);

    VSOut o;
    o.sv = mul(wPos, shadowViewProj);
    o.uv = uv0;
    return o;
}
