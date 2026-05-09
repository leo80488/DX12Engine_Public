// DebugWire.vs.hlsl — simple line vertex shader.
// Reads positions + colors from a ByteAddressBuffer via SV_VertexID.
// Each vertex = float3 pos + uint color (16 bytes).

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;
};

ByteAddressBuffer g_LineVB : register(t2, space0);

struct VSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

VSOut main(uint vid : SV_VertexID)
{
    uint byteOff = vid * 16u; // 16 bytes per vertex (float3 + uint)
    float3 pos;
    pos.x = asfloat(g_LineVB.Load(byteOff));
    pos.y = asfloat(g_LineVB.Load(byteOff + 4u));
    pos.z = asfloat(g_LineVB.Load(byteOff + 8u));
    uint  packedColor = g_LineVB.Load(byteOff + 12u);

    // Unpack RGBA8
    float4 color;
    color.r = float((packedColor >>  0) & 0xFF) / 255.0;
    color.g = float((packedColor >>  8) & 0xFF) / 255.0;
    color.b = float((packedColor >> 16) & 0xFF) / 255.0;
    color.a = float((packedColor >> 24) & 0xFF) / 255.0;

    VSOut o;
    o.pos   = mul(float4(pos, 1.0), viewProj);
    o.color = color;
    return o;
}
