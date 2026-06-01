// VideoComposite.vs.hlsl — fullscreen triangle for NV12 → RGB composite.
//
// Identical math to CloudComposite.vs.hlsl; kept as a dedicated shader so the
// two passes can evolve independently (a future VideoPass may render to a
// sub-rect or world-space quad and only this VS will change).

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2u, vid & 2u);
    o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv.y = 1.0 - o.uv.y;
    return o;
}
