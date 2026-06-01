// Afterimage_Ghost.ps.hlsl — unlit Fresnel-rim ghost PS for snapshot meshes.
//
// Drop-in custom PS for the engine TransparentPass:
//   MaterialComponent.useCustomShader  = true
//   MaterialComponent.customShaderPath = "shaders/Afterimage_Ghost.ps.hlsl"
//   MaterialComponent.userBlendMode    = BlendMode::Additive
//   MaterialComponent.shaderType       = SHADERTYPE_UNLIT
//
// Per-snapshot uniforms travel through MaterialCBVRing → cbuffer at b8.
// AfterimageSystem updates the customParams map every frame:
//   GhostColor   .rgb = HDR tint, .a = lifetime fade (0..1)
//   GhostParams  .x   = fresnel power
//                .y   = base alpha (camera-facing alpha floor)
//                .z   = rim intensity multiplier
//                .w   = unused
//
// Additive PSO uses src.rgb * src_alpha + dst.rgb * 1, so we premultiply
// the rim contribution into rgb to get a stable energy add that fades with
// the silhouette rather than blowing out abruptly.

#include "material.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

StructuredBuffer<MaterialGPUData> g_Materials : register(t2, space0);

cbuffer GhostCB : register(b8, space0)
{
    float4 GhostColor;    // rgb = HDR tint, a = lifetime fade (0..1)
    float4 GhostParams;   // x=fresnelPower, y=baseAlpha, z=rimIntensity, w=_pad
};

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
};

float4 main(PSIn i) : SV_TARGET
{
    const float fresnelPower = max(GhostParams.x, 0.001);
    const float baseAlpha    = saturate(GhostParams.y);
    const float rimIntensity = max(GhostParams.z, 0.0);
    const float lifeFade     = saturate(GhostColor.a);

    // ---- Camera-space view vector ----------------------------------------------
    // g_Materials[materialIndex] gives access to standard material props, but
    // we don't need them — the ghost is purely procedural. Camera position
    // would normally come from the per-frame scene CB; we synthesise the view
    // direction from clip-space derivatives instead so this shader is
    // self-contained (no scene-CB dependency).
    const float3 N = normalize(i.wn);
    // ddx/ddy of world position gives the screen-aligned tangent plane; cross
    // gives the camera forward in world-space (sign depends on winding, but
    // the abs(NdotV) below makes this robust).
    const float3 ddxWP = ddx(i.worldPos);
    const float3 ddyWP = ddy(i.worldPos);
    const float3 V = normalize(cross(ddyWP, ddxWP));

    // ---- Fresnel rim -----------------------------------------------------------
    const float NdotV  = saturate(abs(dot(N, V)));
    const float rim    = pow(1.0 - NdotV, fresnelPower);

    // ---- Compose ---------------------------------------------------------------
    // baseAlpha keeps the body of the mesh faintly visible; rim adds the bright
    // silhouette. Both attenuate by lifeFade so old snapshots dissolve out.
    const float silhouette = baseAlpha + rim * rimIntensity;
    const float alpha      = saturate(silhouette) * lifeFade;

    if (alpha < 0.002) discard;

    const float3 rgb = GhostColor.rgb * alpha;

    return float4(rgb, alpha);
}
