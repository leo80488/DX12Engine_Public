// DebugWire.ps.hlsl — simple solid color output for debug lines.

struct PSIn
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

float4 main(PSIn i) : SV_TARGET
{
    return i.color;
}
