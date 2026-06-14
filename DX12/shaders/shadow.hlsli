#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI

// shadow.hlsli — CSM shadow sampling.
//
// Three biases work in concert to fight self-shadow acne:
//   1. Slope-scaled depth bias (caster side, ras state — ShadowPass.cpp).
//      Pushes stored depth proportional to |dz/dx|,|dz/dy| so steep
//      surfaces get more bias. Per-cascade tuning via cull-mode group.
//   2. Normal offset bias (receiver side, here). Shifts the receiver
//      world-pos along its surface normal before light-space transform,
//      sized in world meters per cascade × saturate(1 - NdotL). Kills
//      grazing-angle acne that slope-scaled alone misses without the
//      "peter pan" of a flat depth bias.
//   3. Receiver-plane depth bias (receiver side, here). Uses ddx/ddy of
//      shadow-space (uv, z) to derive ∂z/∂u, ∂z/∂v on the receiving
//      plane; for each PCF tap it predicts the surface depth at that
//      offset (depthCenter + dot(grad, off)) instead of using the
//      centre depth, so PCF taps over a sloped surface don't self-collide.
//
// Requires the following to be declared before including:
//   - cbuffer with: shadowMatrix[3], cascadeSplits, shadowStrength,
//                   cameraPos, cameraForward, shadowMapTexelSize,
//                   shadowBlendRange, shadowFrameIndex,
//                   cascadeTexelWorldSize, shadowNormalOffset, lightDir
//   - Texture2DArray<float> gShadowCascades : register(t9, space0)
//   - SamplerComparisonState gShadowSampler : register(s2)

static const float kBayer4x4[16] = {
     0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
    12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
     3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
    15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0,
};

// Extract a single cascade's world-space texel size from the float4 packed
// in LightCB (HLSL forbids dynamic indexing of vector components).
float CascadeTexelWorld(int idx)
{
    return idx == 0 ? cascadeTexelWorldSize.x
         : idx == 1 ? cascadeTexelWorldSize.y
         : idx == 2 ? cascadeTexelWorldSize.z
         :            cascadeTexelWorldSize.w;
}

// Receiver-plane depth gradient: how does shadow-space z change per
// shadow-space UV step on the receiving surface? Solving
//   [du/dx du/dy] [dz/du]   [dz/dx]
//   [dv/dx dv/dy] [dz/dv] = [dz/dy]
// via Cramer's rule. Magnitude is clamped — past ~1.0 in (z per uv) we
// are usually on a quad that straddles a depth or cascade discontinuity,
// where the linear-plane assumption breaks and the gradient becomes a
// noise source instead of a bias source.
float2 ReceiverPlaneDepthBias(float3 shadowUVZ)
{
    float3 dx = ddx(shadowUVZ);
    float3 dy = ddy(shadowUVZ);
    float det = dx.x * dy.y - dx.y * dy.x;
    float2 grad;
    grad.x = dy.y * dx.z - dx.y * dy.z;
    grad.y = dx.x * dy.z - dy.x * dx.z;
    float safeDet = (abs(det) > 1e-8) ? det : (det >= 0 ? 1e-8 : -1e-8);
    grad /= safeDet;
    return clamp(grad, float2(-1.0, -1.0), float2(1.0, 1.0));
}

// Vogel-disk 16-tap PCF, rotated per-pixel (Bayer 4×4) + per-frame (mod 4,
// matches TAA's ~8-frame EMA so residual rotation phase doesn't read as
// shadow-edge shimmer on static geometry). Per-tap depth corrected via
// receiver-plane gradient; receiver world-pos pre-shifted along N.
float SampleCascadeShadow(int idx, float3 worldPos, float3 N, float2 screenPos)
{
    // ---- 1) Normal offset bias ---------------------------------------------
    // Push the receiver world-pos along its normal before sampling. Distance
    // scales with this cascade's world-space texel size (visual offset is
    // then constant across cascades) and with (1 - NdotL) — head-on surfaces
    // get no offset (full PCF range available); grazing surfaces get the
    // full push, which is exactly where slope-scaled caster bias falls short.
    const float3 L          = normalize(-lightDir);
    const float  NdotL      = saturate(dot(N, L));
    const float  texelWorld = CascadeTexelWorld(idx);
    const float  noScale    = shadowNormalOffset * texelWorld * (1.0 - NdotL);
    const float3 offsetPos  = worldPos + N * noScale;

    float4 sp = mul(float4(offsetPos, 1.0), shadowMatrix[idx]);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || sp.z < 0.0 || sp.z > 1.0)
        return 1.0;

    const float depthCenter = sp.z;
    const float texelSize   = shadowMapTexelSize;

    // ---- 2) Receiver-plane depth gradient ---------------------------------
    // (∂z/∂u, ∂z/∂v) on the surface, computed once per pixel and reused for
    // every PCF tap below. Lives outside the loop because ddx/ddy on a
    // per-tap quantity is undefined (and pointless — the plane is global).
    const float2 rpdb = ReceiverPlaneDepthBias(float3(uv, depthCenter));

    uint2  px          = uint2(screenPos) & 3u;
    float  bayer       = kBayer4x4[px.x + px.y * 4u];
    float  frameOffset = (float)(shadowFrameIndex & 3u) * (1.0 / 4.0);
    float  baseAngle   = (bayer + frameOffset) * 6.28318530718;

    const float kRadius = 2.5;
    const int   kTaps   = 16;
    const float kGolden = 2.39996323;  // golden angle

    float shadow = 0.0;
    [unroll]
    for (int i = 0; i < kTaps; ++i)
    {
        float  r     = sqrt(((float)i + 0.5) / (float)kTaps);
        float  theta = (float)i * kGolden + baseAngle;
        float2 off   = float2(cos(theta), sin(theta)) * r * kRadius * texelSize;
        // RPDB: predicted surface depth at this tap. Reversed-Z safe because
        // dot(grad, off) tracks the SAME plane the centre sample lies on,
        // so the comparison reference moves with the surface.
        //
        // + shadowBias: constant receiver-side comparison bias (LightCB,
        // 0.0003 NDC). Reversed-Z: RAISING the reference makes the
        // GREATER_EQUAL compare pass more easily → biases toward lit. This
        // was uploaded but never read after the shadow.hlsli rewrite —
        // normal-offset scales by (1 − NdotL) and RPDB only tracks the
        // receiving plane, so neither covers plain rasterization/
        // quantization mismatch between the caster grid and the receiver;
        // that residual is exactly the BACK-cull acne speckle.
        float  depthAtTap = depthCenter + dot(rpdb, off) + shadowBias;
        shadow += gShadowCascades.SampleCmpLevelZero(
            gShadowSampler, float3(uv + off, float(idx)), depthAtTap);
    }
    return shadow * (1.0 / (float)kTaps);
}

// Select cascade by view-Z and return shadow factor [0,1]. Four cascades:
// 0..2 are the standard near cascades, 3 is the ultra-far terrain cascade
// (typically out to ~2000m). Beyond cascade 3's far boundary the receiver
// is treated as fully lit — too distant for shadows to be perceptible.
float ComputeShadowFactor(float3 worldPos, float3 N, float2 screenPos)
{
    if (shadowStrength <= 0.0) return 1.0;

    float viewZ = dot(worldPos - cameraPos, cameraForward);
    if (viewZ > cascadeSplits.w) return 1.0;

    int cascade = 0;
    if      (viewZ > cascadeSplits.z) cascade = 3;
    else if (viewZ > cascadeSplits.y) cascade = 2;
    else if (viewZ > cascadeSplits.x) cascade = 1;

    float shadow = SampleCascadeShadow(cascade, worldPos, N, screenPos);

    // Cross-fade across cascade boundaries. Each cascade's near edge picks
    // the same `shadowBlendRange` width regardless of physical extent —
    // that produces visually constant blend bands across the screen.
    if (cascade < 3 && shadowBlendRange > 0.0)
    {
        float splitFar = (cascade == 0) ? cascadeSplits.x
                       : (cascade == 1) ? cascadeSplits.y
                       :                  cascadeSplits.z;
        float blendT   = saturate((viewZ - (splitFar - shadowBlendRange)) / shadowBlendRange);
        if (blendT > 0.0)
            shadow = lerp(shadow, SampleCascadeShadow(cascade + 1, worldPos, N, screenPos), blendT);
    }

    return lerp(1.0, shadow, shadowStrength);
}

#endif // SHADOW_HLSLI
