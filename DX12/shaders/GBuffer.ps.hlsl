// GBuffer.ps.hlsl — PBR geometry pass pixel shader.
//
// Render target layout (world position is reconstructed from depth in Lighting):
//   TARGET0  albedo    R11G11B10_FLOAT     — baseColor (no emissive — see TARGET5)
//   TARGET1  normal    R16G16B16A16_FLOAT  — world normal packed to [0,1],
//                                             .a = matIdx, encoded with sign
//                                             bit: positive → normal pixel,
//                                             negative → pixel should be
//                                             EXCLUDED from XeGTAO (see
//                                             MAT_FLAG_EXCLUDE_FROM_SSAO).
//                                             Encoding: excluded ⇒ -(matIdx+1);
//                                             matIdx+1 keeps matIdx=0 distinct
//                                             from matIdx=0 non-excluded (±0
//                                             would collide on the sign test).
//   TARGET2  surface   R8G8B8A8_UNORM      — roughness(R), metalness(G), AO(B), reflectance(A)
//   TARGET3  velocity  R16G16_FLOAT        — NDC motion vector
//   TARGET4  extra     R16G16B16A16_FLOAT  — shading-model scratch (SSS thickness, …)
//   TARGET5  sceneCol  R16G16B16A16_FLOAT  — HdrSceneColor: emissive seed for
//                                             Unreal-style additive lighting.
//                                             Lighting pass uses ADDITIVE blend
//                                             so its output adds to this seed,
//                                             producing  finalColor = lighting + emissive
//                                             without emissive ever passing
//                                             through the BRDF.
//
// Surface map input channels: R=AO, G=roughness, B=metalness
// Roughness/metalness are REMAPPED using lerp(min, max, textureValue) where
// min/max come from the material constant buffer (params[0]/params[1]).

#include "material.hlsli"

// Push constants (same b0 as VS — accessible in all shader stages).
cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

// Bindless material buffer — all MaterialGPUData slots packed contiguously.
// Bound at t2 space0 (kSRVSlotBase + 0) by GBufferPass::Execute.
StructuredBuffer<MaterialGPUData> g_Materials : register(t2, space0);

// Per-draw texture slots (fallback, bound by GBufferPass per-draw for non-indirect path).
Texture2D<float4> g_BaseColor  : register(t3, space0);
Texture2D<float4> g_SurfaceMap : register(t4, space0);
Texture2D<float4> g_NormalMap  : register(t5, space0);

// Bindless texture array — all loaded textures indexed by MaterialGPUData.textureHandleIds[].
// Bound once at root param 27 (t0 space2). Requires SM 5.1+.
Texture2D g_AllTextures[] : register(t0, space2);

// Linear-wrap sampler (bound at s0 space0 by GBufferPass::Execute).
SamplerState g_LinearWrap : register(s0, space0);

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;       // per-vertex color (rgb; white when absent)
    float2 uv1       : TEXCOORD3;   // second UV set (0,0 when absent)
    float4 curClip   : TEXCOORD1;   // current clip-space position
    float4 prevClip  : TEXCOORD2;   // previous clip-space position
};

struct GOut
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 surface  : SV_TARGET2;
    float2 velocity : SV_TARGET3;   // screen-space motion vector (NDC units)
    float4 extra    : SV_TARGET4;   // shading-model scratch (SSS thickness etc.)
    float4 sceneCol : SV_TARGET5;   // HdrSceneColor: emissive seed (Unreal-style direct write)
};

#if DOUBLE_SIDED
GOut main(PSIn i, bool isFrontFace : SV_IsFrontFace)
#else
GOut main(PSIn i)
#endif
{
    MaterialGPUData mat = g_Materials[materialIndex];

    float4 baseColor        = mat.baseColor;
    float  roughnessMin     = mat.roughnessMin;
    float  roughnessMax     = mat.roughnessMax;
    float  metalnessMin     = mat.metalnessMin;
    float  metalnessMax     = mat.metalnessMax;
    float  reflectance      = mat.reflectance;
    float  normalStrength   = mat.normalStrength;

    // Fallback direct values when no material params have been uploaded yet.
    bool useFallback = (mat.paramCount == 0);
    if (useFallback)
    {
        baseColor      = float4(i.col, 1.0);
        roughnessMin   = 0.0;   roughnessMax  = 0.5;
        metalnessMin   = 0.0;   metalnessMax  = 0.0;
        reflectance    = 0.04;
        normalStrength = 1.0;
    }

    // Sample textures: use bindless path (textureHandleIds) if valid, fallback to per-draw bound.
    int texBaseColor = mat.textureHandleIds[0]; // BASECOLORMAP
    int texSurface   = mat.textureHandleIds[2]; // SURFACEMAP
    int texNormal    = mat.textureHandleIds[1]; // NORMALMAP

    if (texBaseColor >= 0)
        baseColor *= g_AllTextures[texBaseColor].Sample(g_LinearWrap, i.uv);
    else
        baseColor *= g_BaseColor.Sample(g_LinearWrap, i.uv);

    // Per-vertex color tint — opt-in per material (MaterialComponent::
    // USE_VERTEXCOLORS). i.col is white for meshes without a color stream, so
    // this is harmless there. Skipped on the fallback path, which already set
    // baseColor = float4(i.col, 1) so vertex color isn't applied twice.
    if (!useFallback && (mat.materialFlags & MAT_FLAG_USE_VERTEXCOLOR))
        baseColor.rgb *= i.col;

#if ALPHA_TEST
    // Ben Golus "Anti-aliased Alpha Test": rescale alpha so the threshold
    // sits at 0.5 with a one-pixel-wide soft ramp derived from screen-space
    // alpha derivatives. TAA's per-frame jitter samples different sub-pixel
    // positions along that ramp → the binary hard-edged clip becomes a soft
    // edge that TAA integrates into a proper anti-aliased silhouette.
    //
    // Without this, distant alpha-tested geometry (fences, chain-link, leaf
    // cards) is a binary coverage signal that no amount of history-blending
    // can resolve — each pixel is hard-on or hard-off per frame → flicker.
    {
        float a      = baseColor.a;
        float ref    = mat.alphaRef;
        float width  = max(fwidth(a), 1e-4);
        float scaled = (a - ref) / width + 0.5;
        clip(scaled);
        baseColor.a = saturate(scaled);
    }
#endif

    float4 surface;
    if (texSurface >= 0)
        surface = g_AllTextures[texSurface].Sample(g_LinearWrap, i.uv);
    else
        surface = g_SurfaceMap.Sample(g_LinearWrap, i.uv);

    float ao        = surface.r;
    float roughness = lerp(roughnessMin, roughnessMax, surface.g);
    float metalness = lerp(metalnessMin, metalnessMax, surface.b);

    float2 rg;
    if (texNormal >= 0)
        rg = g_AllTextures[texNormal].Sample(g_LinearWrap, i.uv).rg * 2.0 - 1.0;
    else
        rg = g_NormalMap.Sample(g_LinearWrap, i.uv).rg * 2.0 - 1.0;
    float  nz       = sqrt(saturate(1.0 - dot(rg, rg)));
    float3 tsNormal = float3(rg, nz);
    // Scale XY by normalStrength (Z stays derived → softer effect as strength → 0).
    tsNormal.xy *= normalStrength;
    tsNormal = normalize(tsNormal);

    // Build TBN matrix and transform to world space.
    float3 N  = normalize(i.wn);
    float3 T  = normalize(i.wt);
    float3 BT = normalize(i.wbt);
#if DOUBLE_SIDED
    // Back-face: flip geometry normal and bitangent so normals face the camera.
    if (!isFrontFace) { N = -N; BT = -BT; }
#endif
    float3x3 TBN = float3x3(T, BT, N);  // rows = tangent, bitangent, normal
    float3 worldNormal = normalize(mul(tsNormal, TBN));

    // ---- Geometric specular anti-aliasing (Kaplanyan & Hable 2016) ---------
    // Measure sub-pixel normal variation via screen-space derivatives, then
    // widen the GGX lobe (work in alpha = roughness^2) by exactly the amount
    // needed to cover that variance. Result: rough-but-not-mirror surfaces at
    // distance stop producing random single-pixel specular sparkles because
    // the BRDF lobe is now broad enough that neighbouring sub-pixel normals
    // all fall inside it, turning "lucky pixel hits glint / unlucky pixel
    // misses" into a stable averaged highlight. Kappa caps the broadening at
    // silhouettes where derivatives spike (adjacent pixel belongs to distant
    // geometry) so we don't over-roughen edges.
    //
    // Constants match the Unreal / Frostbite shipping values: sigma^2 = 0.25
    // (screen-space kernel variance), kappa = 0.18 (saturation cap).
    {
        const float kSigma2 = 0.25;
        const float kKappa  = 0.18;
        float3 dndu     = ddx(worldNormal);
        float3 dndv     = ddy(worldNormal);
        float  variance = kSigma2 * (dot(dndu, dndu) + dot(dndv, dndv));
        float  kernelR2 = min(2.0 * variance, kKappa);
        float  alpha    = roughness * roughness;
        float  alpha2   = saturate(alpha + kernelR2);
        roughness       = sqrt(alpha2);
    }

    // Compute screen-space velocity (NDC delta between current and previous frame).
    float2 curNDC  = i.curClip.xy  / i.curClip.w;
    float2 prevNDC = i.prevClip.xy / i.prevClip.w;
    float2 velocity = curNDC - prevNDC;  // NDC units [-2, +2] range

    // Sample emissive map and multiply by emissive color & strength.
    //
    // Unreal-style: emissive is written DIRECTLY to HdrSceneColor (RT5).
    // The lighting pass uses ADDITIVE blend so its computed lighting is
    // added on top, producing finalColor = lighting + emissive without
    // emissive ever passing through the BRDF (which used to dim it by
    // diffuse_brdf × NdotL × shadow + ambient × ao — so a strength-16
    // emissive in shadow could end up at 0.8 in the final image).
    float4 emissiveParam = mat.emissiveColor; // .rgb = color, .w = strength
    float3 emissiveOut   = emissiveParam.rgb * emissiveParam.w;
    int texEmissive = mat.textureHandleIds[MAT_TEX_EMISSIVEMAP];
    if (texEmissive >= 0)
        emissiveOut *= g_AllTextures[texEmissive].Sample(g_LinearWrap, i.uv).rgb;

    GOut o;
    // RT0 albedo: pure baseColor (no emissive folded in). The emissive seed
    // sits in RT5 instead and reaches the screen via additive lighting blend.
    o.albedo   = baseColor;
    // Encode SSAO-exclusion into the sign of normal.a (see layout comment at
    // top of file). Lighting.ps.hlsl strips the sign to recover matIdx;
    // XeGTAO.cs.hlsl only looks at the sign.
    const bool excludeSSAO = (mat.materialFlags & MAT_FLAG_EXCLUDE_FROM_SSAO) != 0;
    const float matIdxEnc  = excludeSSAO ? -(float(materialIndex) + 1.0)
                                         :  float(materialIndex);
    o.normal   = float4(worldNormal * 0.5 + 0.5, matIdxEnc);
    o.surface  = float4(roughness, metalness, ao, reflectance);
    o.velocity = velocity;
    // RT4 extra: shading-model scratch (SSS thickness, clearcoat, …).
    // Default GBuffer PS writes 0; custom shaders override.
    o.extra    = float4(0, 0, 0, 0);
    // RT5 sceneCol: emissive seed for additive lighting. Always emissive only —
    // Unlit's baseColor is delivered by Lighting.ps's Unlit variant returning
    // float4(albedo, 1) which gets added on top via the additive blend.
    o.sceneCol = float4(emissiveOut, 1.0);
    return o;
}

