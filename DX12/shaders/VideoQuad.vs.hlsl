// VideoQuad.vs.hlsl — world-space quad VS for VideoQuadPass.
//
// Procedural unit quad centred on origin in local XY (Z = 0). The CB carries
// the per-draw world matrix already scaled by worldWidth × worldHeight, and a
// view-projection matrix shared with the scene camera (un-jittered so the
// playback panel never shimmers under TAA).
//
// SV_VertexID lookup:
//   0 = (-0.5, -0.5)   3 = ( 0.5, -0.5)
//   1 = (-0.5,  0.5)   4 = ( 0.5,  0.5)
//   2 = ( 0.5, -0.5)   5 = (-0.5,  0.5)
// → two triangles, CCW when looking from +Z.

// row_major: matrices arrive as XMFLOAT4X4 written row-major from C++ —
// matches `mul(rowVec, mat)` semantics already used by other engine shaders
// (PerViewCB.viewProj in DebugWire.vs.hlsl, CloudRaymarch invViewProj, ...).
cbuffer VideoQuadCB : register(b2, space0)
{
    row_major float4x4 worldMatrix;     // entity GlobalTransform (no scale)
    row_major float4x4 viewProjMatrix;  // un-jittered scene viewProj
    float    quadWidth;
    float    quadHeight;
    float    alpha;
    uint     colorSpace;
};

static const float2 kQuad[6] = {
    float2(-0.5, -0.5),
    float2(-0.5,  0.5),
    float2( 0.5, -0.5),
    float2( 0.5, -0.5),
    float2( 0.5,  0.5),
    float2(-0.5,  0.5),
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    float2 local = kQuad[vid];

    // UV: 0..1 across the quad. Local (-0.5, -0.5) → (0, 1) so the texture's
    // top row maps to the quad's top edge (matching screen-space convention).
    float2 uv = local + 0.5;
    uv.y = 1.0 - uv.y;

    float3 localPos = float3(local.x * quadWidth, local.y * quadHeight, 0.0);
    float4 worldPos = mul(float4(localPos, 1.0), worldMatrix);
    float4 clipPos  = mul(worldPos, viewProjMatrix);

    VSOut o;
    o.pos = clipPos;
    o.uv  = uv;
    return o;
}
