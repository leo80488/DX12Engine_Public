// Skybox.vs.hlsl — vertex shader for the skybox cube.
//
// Reads 36 pre-expanded float3 positions from a ByteAddressBuffer at t2.
// The w=0 trick: mul(float4(pos, 0.0f), viewProj).xyww sets NDC z = w = 1 (far plane).
// Combined with LESS_EQUAL depth test the sky draws only where no geometry rendered.
// The local cube position IS the cubemap sampling direction (world rotation, no translation).

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;  // transpose(view * proj), row-vector convention
};

// Cube vertex positions — 36 float3 (ByteAddressBuffer, 12 bytes per vertex)
ByteAddressBuffer gCubeVB : register(t2, space0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 dir : TEXCOORD0;  // cubemap sampling direction
};

VSOut main(uint vid : SV_VertexID)
{
    // Load float3 position (12 bytes per vertex)
    uint byteOffset = vid * 12u;
    float3 localPos;
    localPos.x = asfloat(gCubeVB.Load(byteOffset     ));
    localPos.y = asfloat(gCubeVB.Load(byteOffset + 4u));
    localPos.z = asfloat(gCubeVB.Load(byteOffset + 8u));

    // w=0 strips camera translation from the view matrix so the skybox stays centred.
    // Reversed-Z: far plane is at NDC z=0, so force clipPos.z = 0 (dividing by w
    // keeps it at 0). Depth compare is GREATER_EQUAL against the cleared 0.
    float4 clipPos = mul(float4(localPos, 0.0f), viewProj);
    clipPos.z = 0.0f; // force z/w = 0 (far plane under reversed Z)

    VSOut o;
    o.pos = clipPos;
    o.dir = localPos; // local cube position = world direction (no translation, only rotation)
    return o;
}
