// Trail.ps.hlsl — outputs the interpolated ribbon color with age fade.
// No textures, no radial falloff — the ribbon itself is already thin
// enough that a pure color is fine for MVP.

struct VSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float  alpha : TEXCOORD0;
};

float4 main(VSOut i) : SV_TARGET
{
    float4 c = i.color;
    c.a *= i.alpha;
    if (c.a < 0.004) discard;
    return c;
}
