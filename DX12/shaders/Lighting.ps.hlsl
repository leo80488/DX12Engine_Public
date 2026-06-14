// Lighting.ps.hlsl — deferred PBR + NPR + Unlit lighting pass with Clustered Deferred Shading.
//
// Output blend: ADDITIVE (configured in LightingPass.cpp). HdrSceneColor was
// pre-seeded with material emissive by GBufferPass (Unreal-style direct
// emissive write). This shader therefore returns ONLY lit contribution
// (lighting result, NOT lighting + emissive) and the blend produces
//   SceneColor = lighting + emissive
// where emissive bypasses the BRDF.
//
// Compiled as three variants via permutation:
//   NPR_PASS=0, UNLIT=0 (default): standard PBR Cook-Torrance
//   NPR_PASS=1:                    NPR ramp-based diffuse + physical specular
//                                  (both SHADER_NPR_RAMP texture-ramp and
//                                   SHADER_NPR_COLOR color-ramp branch at runtime
//                                   via MaterialGPUData.shaderType)
//   UNLIT=1:                       output baseColor only (additive blend
//                                  combines with emissive seed for final color)
// Stencil test ensures each variant only processes matching pixels
// (PBR=1, NPR=2, Unlit=3).

#include "material.hlsli"
#include "brdf.hlsli"
#include "sky_sh.hlsli"            // gSkySH SRV + EvalSH2
#include "reflection_probe.hlsli"  // ReflectionProbe struct,
                                   // gReflectionProbeArray / gReflectionProbes SRVs,
                                   // ParallaxCorrectAABB / ComputeProbeWeight helpers

// LightCB — fullscreen lighting PS owns b1 (no PerViewCB on this draw).
#define LIGHT_CB_REGISTER b1
#include "light_cb.hlsli"
#include "view_mode_common.hlsli"   // VIEW_MODE_* + WIREFRAME_COLOR (LightCB.viewMode)

// GBuffer inputs
Texture2D<float4> gAlbedo   : register(t2, space0);
Texture2D<float4> gNormal   : register(t3, space0);
Texture2D<float4> gSurface  : register(t4, space0);
Texture2D<float>  gDepth    : register(t5, space0);

// IBL
TextureCube       gIrradiance : register(t6, space0);
TextureCube       gRadiance   : register(t7, space0);
Texture2D<float2> gBRDFLUT    : register(t8, space0);

// Per-cluster probe list — fed by ClusterCullProbes.cs.hlsl. Lighting-only
// (TransparentPass iterates all probes without the cluster lookup), so this
// pair stays inline rather than going into reflection_probe.hlsli.
struct ProbeGridEntry
{
    uint offset;
    uint count;
};
StructuredBuffer<ProbeGridEntry>  gReflectionProbeGrid  : register(t25, space0);
StructuredBuffer<uint>            gReflectionProbeIndex : register(t26, space0);

// SSR resolved reflection (1-frame latent). .rgb = reflection color (pre-
// Fresnel), .a = confidence. Lighting.ps reads confidence only — the
// composite step (Phase 4.7) handles the Fresnel/env-BRDF apply and the
// additive blend. `ssrConf == 0` path is identical to pre-SSR behaviour.
Texture2D<float4>                 gSSRResult            : register(t27, space0);

// DDGI bindings — multi-volume. Each per-resource array holds DDGI_MAX_VOLUMES
// SRVs in slot-index order (matches DDGIVolumeManager's per-slot writes), and
// gDDGIVolumes is also slot-indexed: gDDGIVolumes[s].flags & 1u gates whether
// slot s holds an active volume. Inactive slots have null SRVs (valid descriptors,
// never read because the (flags & 1u) gate skips the iteration).
//
// Register layout MUST match the root signature in GraphicsDX12::CreateDefaultRootSignature():
//   t28           — gDDGIVolumes        (StructuredBuffer<DDGIVolumeGPU>)
//   t29..t32      — gDDGIProbeSH[4]
//   t33..t36      — gDDGIDepth[4]
//   t37..t40      — gDDGIProbeData[4]
#include "DDGISampling.hlsli"
StructuredBuffer<DDGIVolumeGPU>   gDDGIVolumes      : register(t28, space0);
StructuredBuffer<DDGIProbeSH>     gDDGIProbeSH[DDGI_MAX_VOLUMES]   : register(t29, space0);
Texture2D<float2>                 gDDGIDepth[DDGI_MAX_VOLUMES]     : register(t33, space0);
StructuredBuffer<DDGIProbeData>   gDDGIProbeData[DDGI_MAX_VOLUMES] : register(t37, space0);

// Aerial Perspective 3D LUT (Hillaire 2020). xy = screen uv, z = quadratic
// depth slice 0..kApMaxDist. rgb = pre-multiplied inscatter, a = transmittance.
Texture3D<float4> gAerialPerspective : register(t20, space0);

#if NPR_PASS
// NPR ramp texture (only compiled into NPR variant)
Texture2D<float4> gRampTex    : register(t16, space0);
#endif

// CSM shadow cascade maps
Texture2DArray<float> gShadowCascades : register(t9, space0);

SamplerState           gSampler      : register(s0);
SamplerState           gIBLSampler   : register(s1);
SamplerComparisonState gShadowSampler: register(s2);

// GBuffer "extra" slot (RT4). Historically held pre-multiplied emissive; now
// emissive is baked into albedo (RT0) so this slot is reserved for shading-
// model-specific per-pixel data (SSS thickness, clearcoat params, etc.).
// Default GBuffer PS clears it to zero — custom shaders opt-in by writing.
Texture2D<float4> gExtraGBuffer : register(t17, space0);

// Screen-space AO (XeGTAO, one-frame latency). 1.0 = fully lit, 0.0 = fully occluded.
Texture2D<float>  gSSAO      : register(t18, space0);

// Per-material data (for per-pixel NPR params via materialIndex stored in normal.a)
StructuredBuffer<MaterialGPUData> gMaterials : register(t12, space0);

// Clustered lighting
#include "cluster_common.hlsli"
StructuredBuffer<GPULight>        gLights         : register(t13, space0);
StructuredBuffer<uint>            gLightIndexList : register(t14, space0);
StructuredBuffer<LightGridEntry>  gLightGrid      : register(t15, space0);

// Per-spot-light shadow maps (opt-in via LightData::castsShadow).
// Atlas: Texture2DArray<float> of D32_FLOAT slices, reversed-Z (closer to light = larger z).
// VPs  : StructuredBuffer<float4x4> — world → light clip, one per active caster slice.
Texture2DArray<float>       SpotShadowAtlas : register(t21, space0);
StructuredBuffer<float4x4>  SpotShadowVPs   : register(t22, space0);

float SampleSpotShadow(float3 worldPos, uint sliceIdx)
{
    float4 sc = mul(float4(worldPos, 1.0), SpotShadowVPs[sliceIdx]);
    if (sc.w <= 0.0) return 1.0;
    sc.xyz /= sc.w;
    float2 uv = sc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (any(uv < 0.0) || any(uv > 1.0) || sc.z < 0.0 || sc.z > 1.0)
        return 1.0;
    // Reversed-Z GREATER_EQUAL compare. Self-shadow acne is already handled
    // on the CASTER side (rasterizer slope-scaled depth bias in
    // SpotShadowPass::BuildPSODesc). Adding a positive receiver bias on top
    // stacks the bias in the same direction and causes visible light leaks
    // past thin occluders — drop it to zero and rely on the rasterizer side.
    return SpotShadowAtlas.SampleCmpLevelZero(gShadowSampler,
             float3(uv, (float)sliceIdx), sc.z);
}

// Per-point-light omnidirectional (cubemap) shadow (opt-in via
// LightData::castsShadow). One cube (6 faces) per active caster, addressed by
// light.shadowSliceIdx (the SAME field spot lights use for their atlas slice —
// a light is point XOR spot, so the union is unambiguous). Reversed-Z depth.
//
// Faces are rendered by PointShadowPass with a 90° perspective per face,
// NearZ = radius, FarZ = POINT_SHADOW_NEAR (mirrors the SpotShadowPass reversed-Z
// convention). The hardware cube sampler picks the face + texel from the
// world-space direction; we reconstruct the matching reversed-Z reference depth
// analytically from the fragment's distance — no per-light VP matrix needed.
TextureCubeArray<float> PointShadowAtlas : register(t41, space0);

// Must match PointShadowPass.h (kShadowMapSize, kNearPlane).
#define POINT_SHADOW_SIZE 1024.0
#define POINT_SHADOW_NEAR 0.05

float SamplePointShadow(float3 worldPos, float3 N, float3 lightPos, float radius, uint cubeIdx)
{
    float3 v    = worldPos - lightPos;
    float  dist = length(v);
    if (dist >= radius) return 1.0;   // outside range → lit (attenuation already culls)

    // Normal-offset bias (world space, scales with the face's texel size at this
    // distance) to fight self-shadow acne. All depth bias is on the caster side
    // (rasterizer slope-scale), matching SpotShadow — this is the only receiver
    // nudge, and it pushes ALONG the normal so it never leaks past occluders.
    // Scale by (1 - NdotL) like SampleCascadeShadow (shadow.hlsli): head-on
    // surfaces get zero offset (tight contact), grazing get the full push.
    float  texelW  = 2.0 * dist / POINT_SHADOW_SIZE;
    float3 Ldir    = -v / dist;                          // toward the light
    float  noScale = saturate(1.0 - dot(N, Ldir));
    float3 sp      = worldPos + N * texelW * 2.0 * noScale;
    float3 vv      = sp - lightPos;

    // View-space depth on the selected cube face == dominant axis magnitude.
    // Reversed-Z NDC for PerspectiveFovLH(NearZ = radius, FarZ = POINT_SHADOW_NEAR):
    //   ndc(Vz) = fRange * (1 - radius / Vz),  fRange = near / (near - radius)
    float  Vz       = max(abs(vv.x), max(abs(vv.y), abs(vv.z)));
    Vz = max(Vz, 1e-3);
    float  fRange   = POINT_SHADOW_NEAR / (POINT_SHADOW_NEAR - radius);
    float  refDepth = saturate(fRange * (1.0 - radius / Vz));

    return PointShadowAtlas.SampleCmpLevelZero(gShadowSampler,
             float4(vv, (float)cubeIdx), refDepth);
}

#include "shadow.hlsli"
#if NPR_PASS
#include "npr_ramp.hlsli"
#endif

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

// ---------------------------------------------------------------------------
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc;
    ndc.x =  uv.x * 2.0f - 1.0f;
    ndc.y =  1.0f - uv.y * 2.0f;
    float4 clipPos = float4(ndc.x, ndc.y, depth, 1.0f);
    float4 worldPos = mul(clipPos, invViewProj);
    worldPos.xyz /= worldPos.w;
    return worldPos.xyz;
}

// ---------------------------------------------------------------------------
float4 main(PSIn i) : SV_TARGET
{
    float3 albedo    = gAlbedo.Sample(gSampler, i.uv).rgb;

    // --- Global view-mode override (Unity/Unreal-style viewmode switch) ------
    // Driven by LightCB.viewMode. Both branches bypass the BRDF entirely:
    //   Unlit     → flat base color; its emissive still arrives via the pass's
    //               additive blend onto the GBuffer-seeded HdrSceneColor.
    //   Wireframe → constant teal. Geometry PSOs rasterize edges only
    //               (FILL_MODE_WIREFRAME) and only edge pixels carry a stencil
    //               id, so only edges reach this stencil-gated draw → teal
    //               lines over the (skybox-suppressed) black background.
    if (viewMode == VIEW_MODE_UNLIT)
        return float4(albedo, 1.0);
    if (viewMode == VIEW_MODE_WIREFRAME)
        return float4(WIREFRAME_COLOR, 1.0);

    float4 normalRaw = gNormal.Sample(gSampler, i.uv);
    float3 N         = normalize(normalRaw.rgb * 2.0 - 1.0);
    // normal.a holds matIdx with its sign bit repurposed by GBuffer.ps.hlsl as
    // the MAT_FLAG_EXCLUDE_FROM_SSAO marker (negative value ⇒ excluded pixel,
    // encoded as -(matIdx+1) so matIdx=0 stays distinguishable from +0).
    // Lighting itself doesn't care about the flag — strip the sign and the +1
    // offset to recover the original matIdx.
    const float aRaw  = normalRaw.a;
    uint matIdx       = (aRaw < 0.0) ? (uint)(-aRaw - 1.0) : (uint)aRaw;

    // Phase E: custom-material shading-model runtime routing. Standard stays
    // on the PBR path below; Unlit early-exits with baseColor only — its
    // emissive arrives via additive blend onto the GBuffer-seeded
    // HdrSceneColor (Unreal-style direct emissive write). Other models
    // (ClearCoat / Subsurface / Anisotropic) fall through to PBR for now —
    // real BRDFs are TODO once the base path is stable.
    const uint runtimeShadingModel = gMaterials[matIdx].customShadingModel;
    if (runtimeShadingModel == SHADING_MODEL_UNLIT)
    {
        return float4(albedo, 1.0);
    }

#if UNLIT
    // --- Unlit variant: output baseColor; emissive arrives via additive blend ---
    return float4(albedo, 1.0);
#else
    float4 surface   = gSurface.Sample(gSampler, i.uv);
    float  depth     = gDepth.Sample(gSampler, i.uv);
    float3 worldPos  = ReconstructWorldPos(i.uv, depth);

    float roughness   = max(surface.r, 0.045);
    float metalness   = surface.g;
    float bakedAO     = surface.b;
    float ssao        = gSSAO.Sample(gSampler, i.uv).r;
    float ao          = bakedAO * ssao;    // combine baked + screen-space AO
    float reflectance = surface.a;

    float3 V     = normalize(cameraPos - worldPos);
    float3 L     = normalize(-lightDir);
    float  NdotL = saturate(dot(N, L));
    float  NdotV = saturate(dot(N, V));

    float  f0Scalar = 0.16 * reflectance * reflectance;
    float3 F0 = lerp(float3(f0Scalar, f0Scalar, f0Scalar), albedo, metalness);

    // Cook-Torrance BRDF for directional light
    float3 specular, kD;
    EvalCookTorrance(N, V, L, F0, roughness, metalness, albedo, specular, kD);

    float  shadowFactor = ComputeShadowFactor(worldPos, N, i.pos.xy);
    // Per-material receive-shadow opt-out (MaterialComponent::DISABLE_RECEIVE_SHADOW).
    // Lookup is already in the material cbuffer we read above for NPR routing.
    if (gMaterials[matIdx].materialFlags & MAT_FLAG_DISABLE_RECEIVE_SHADOW)
        shadowFactor = 1.0;

    // ---- Directional light (PBR for both paths) ----
    float3 diffuse = kD * albedo / PI;
    float3 Lo = (diffuse + specular) * lightColor * NdotL * shadowFactor;

#if NPR_PASS
    // NPR accumulation — split into directional vs. ambient so IBL keeps its color.
    //   nprLightEnergy   : scalar directional-light intensity (goes through ramp).
    //   nprAmbientColor  : full-color ambient diffuse (IBL irradiance, no ramp) —
    //                      preserves environment hue so cool IBL still cools skin
    //                      exactly like PBR (was collapsing to luminance before,
    //                      which made NPR look redder than PBR under cool IBL).
    //   nprSpecularAccum : full-color specular (unchanged — stays physical).
    float  nprLightEnergy   = Luminance(kD * lightColor) * NdotL * shadowFactor / PI;
    float3 nprAmbientColor  = float3(0, 0, 0);
    float3 nprSpecularAccum = specular * lightColor * NdotL * shadowFactor;
#endif

    // ---- Clustered point/spot lights ----
    if (clusterLightCount > 0)
    {
        float  viewZ  = abs(mul(float4(worldPos, 1.0), viewMatrix).z);
        uint3  coord  = GetClusterCoord(i.uv, viewZ, clusterNearZ, clusterFarZ);
        uint   key    = ClusterIndex(coord);
        uint   offset = gLightGrid[key].offset;
        uint   count  = gLightGrid[key].count;

        for (uint li = 0; li < count; li++)
        {
            uint     idx   = gLightIndexList[offset + li];
            GPULight light = gLights[idx];
            if (light.type == 0) continue;

            float3 Lp   = light.position - worldPos;
            float  dist = length(Lp);
            if (dist > light.radius) continue;

            float3 Ll   = Lp / max(dist, 1e-6);
            float  NdL  = saturate(dot(N, Ll));
            if (NdL <= 0.0) continue;

            float  distNorm  = dist / light.radius;
            float  atten     = saturate(1.0 - distNorm * distNorm);
            atten *= atten;
            atten /= max(dist * dist, 0.01);

            float spotFactor = 1.0;
            if (light.type == 2 && light.spotAngle > 0.0)
            {
                float cosAngle = dot(-Ll, normalize(light.direction));
                float cosOuter = cos(light.spotAngle);
                float cosInner = cos(light.spotAngle * 0.75);
                spotFactor = smoothstep(cosOuter, cosInner, cosAngle);
            }

            float3 radiance = light.color * light.intensity * atten * spotFactor;

            // Opt-in spot shadow: when the light owns a slice in the atlas, modulate
            // radiance by the comparison-sampled visibility term.
            if (light.type == 2 && light.shadowSliceIdx != 0xFFFFFFFFu)
                radiance *= SampleSpotShadow(worldPos, light.shadowSliceIdx);
            // Opt-in point shadow: omnidirectional cube sampled by direction.
            else if (light.type == 1 && light.shadowSliceIdx != 0xFFFFFFFFu)
                radiance *= SamplePointShadow(worldPos, N, light.position, light.radius,
                                              light.shadowSliceIdx);

            float3 specL, kDL;
            EvalCookTorrance(N, V, Ll, F0, roughness, metalness, albedo, specL, kDL);

            float3 diff = kDL * albedo / PI;
            float3 diffContrib = diff * radiance * NdL;
            float3 specContrib = specL * radiance * NdL;
            Lo += diffContrib + specContrib;

#if NPR_PASS
            // Scalar light energy (no albedo) for the NPR diffuse rebuild.
            nprLightEnergy   += Luminance(kDL * radiance) * NdL / PI;
            nprSpecularAccum += specContrib;
#endif
        }
    }

    // ---- IBL (Image-Based Lighting) ----
    // Indirect diffuse path (plan §5.1 fallback chain):
    //   1. DDGI volume sample (preferred — physically integrates indirect bounces)
    //   2. Sky IBL diffuse (fallback for volume-uncovered regions / disabled DDGI)
    // The shader composes them as (DDGI * coverage) + (SkyIBLDiff * (1 - coverage))
    // so a fully-covered point gets pure DDGI and a far-out point gets pure sky.
    //
    // Knob ownership (2026-05-23 — decoupled DDGI from iblStrength):
    //   iblStrength         — master gate on SKY-DERIVED indirect only (sky
    //                         IBL diffuse + reflection-probe/sky specular).
    //                         Setting it to 0 removes the visible sky's
    //                         contribution but DDGI keeps running.
    //   ddgiDiffuseScale    — DDGI diffuse master. Independent of iblStrength.
    //                         DDGI is its own indirect path; the sky's master
    //                         gate must not silence it.
    //   skyIBLDiffuseScale  — per-source weight on sky diffuse (composed
    //                         before iblStrength, only applies where DDGI
    //                         coverage < 1).
    float3 iblContrib = float3(0, 0, 0);
    if (iblRadianceMips > 0)
    {
        // ---- Indirect diffuse: DDGI → Sky IBL ----
        // Multi-volume: iterate every slot 0..DDGI_MAX_VOLUMES-1, gate per-slot
        // on (flags & 1u). Each volume contributes its sampled irradiance
        // weighted by its own AABB fade weight (DDGI_VolumeFadeWeight, returned
        // in the .a channel). Overlapping volumes accumulate proportionally;
        // total coverage saturates at 1.0 so the (1-coverage) sky-IBL fallback
        // contributes only where no volume reaches.
        float3 ddgiIrradiance = float3(0, 0, 0);
        float  ddgiCoverage   = 0.0;
        if (ddgiEnabled != 0 && ddgiVolumeCount > 0)
        {
            float3 sumIrr = 0;
            float  sumW   = 0;
            [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
            {
                DDGIVolumeGPU vol = gDDGIVolumes[vi];
                if ((vol.flags & 1u) == 0u) continue; // inactive slot
                float4 s = DDGI_SampleVolume(worldPos, N, V, vol,
                                             gDDGIProbeSH[vi], gDDGIDepth[vi],
                                             gDDGIProbeData[vi], gIBLSampler);
                // s.a = volume fade weight; weight irradiance by it so volumes
                // closer to their AABB centre dominate the blend.
                sumIrr += s.rgb * s.a;
                sumW   += s.a;
            }
            if (sumW > 1e-4)
            {
                ddgiIrradiance = sumIrr / sumW;
                ddgiCoverage   = saturate(sumW);
            }
        }

        float3 skyDiff = (iblUseSH != 0) ? EvalSH2(N)
                                         : gIrradiance.Sample(gIBLSampler, N).rgb;

        // ---- AO split (plan §3.5: avoid double-occlusion) -------------------
        // DDGI already encodes mid-scale (probe-spacing) visibility via the
        // probe-distance test, so re-applying full screen-space AO on the DDGI
        // path stacks the same occlusion twice and crushes interiors.
        // ddgiAONearFieldStrength lerps AO toward 1.0 for the DDGI portion
        // only — Sky-IBL fallback keeps full AO since SH/cube-irradiance has
        // no built-in visibility.
        float ddgiAO = lerp(1.0, ao, saturate(ddgiAONearFieldStrength));
        // iblStrength only gates SKY-derived contributions; DDGI controls its
        // own intensity via ddgiDiffuseScale (decoupled from the sky master gate).
        float3 ddgiDiffPart = ddgiIrradiance * ddgiDiffuseScale * ddgiCoverage * ddgiAO;
        float3 skyDiffPart  = skyDiff * skyIBLDiffuseScale * saturate(1.0 - ddgiCoverage) * ao * iblStrength;
        float3 iblDiffuse   = ddgiDiffPart + skyDiffPart;
        float3 R            = reflect(-V, N);
        float  mip          = roughness * float(iblRadianceMips - 1);

        // ---- Specular IBL: weighted reflection-probe accumulation -----------
        // Probe loop now reads the per-cluster probe list emitted by
        // ClusterCullProbes.cs — empty cluster => no probe samples (sky path
        // takes 100% of the weight). reflectionProbeCount > 0 gates the
        // cluster lookup so single-camera scenes that have probes but no
        // cluster pass wired still skip cleanly.
        float3 probeAccum  = float3(0, 0, 0);
        float  probeWeight = 0.0;

        if (reflectionProbeCount > 0)
        {
            float  probeViewZ  = abs(mul(float4(worldPos, 1.0), viewMatrix).z);
            uint3  probeCoord  = GetClusterCoord(i.uv, probeViewZ, clusterNearZ, clusterFarZ);
            uint   probeKey    = ClusterIndex(probeCoord);
            uint   probeOffset = gReflectionProbeGrid[probeKey].offset;
            uint   probeLocal  = gReflectionProbeGrid[probeKey].count;

            for (uint pli = 0; pli < probeLocal && probeWeight < 0.999; ++pli)
            {
                uint pi = gReflectionProbeIndex[probeOffset + pli];
                ReflectionProbe probe = gReflectionProbes[pi];
                float w = ComputeProbeWeight(worldPos, probe) * (1.0 - probeWeight);
                if (w <= 1e-3) continue;

                float3 Rcorr   = ParallaxCorrectAABB(R, worldPos, probe.position,
                                                     probe.boxMin, probe.boxMax);
                float4 sample4 = gReflectionProbeArray.SampleLevel(gIBLSampler,
                                     float4(Rcorr, (float)probe.cubemapSlice), mip);
                // Per-probe intensity scales the radiance but NOT the coverage
                // weight (probeWeight stays the sky-fallback blend factor).
                probeAccum  += sample4.rgb * w * probe.intensity;
                probeWeight += w;
            }
        }

        // Sky fallback for any pixel not fully covered by probes.
        // Knob split (2026-06-09): probe specular (probeAccum, already × per-probe
        // intensity) is INDEPENDENT of iblStrength so local reflection probes
        // light surfaces even when the sky/atmosphere master gate is 0. Only the
        // sky-cube specular fallback carries iblStrength.
        float3 skySpecular  = gRadiance.SampleLevel(gIBLSampler, R, mip).rgb;
        // Square the sky-fill so a probe-covered surface suppresses the bright
        // sky reflection more aggressively (interiors shouldn't pick up sky at
        // the probe box-fade edge). At full coverage probeWeight→1 → no sky.
        float  skyFill      = saturate(1.0 - probeWeight);
        float3 specRadiance = probeAccum + skySpecular * (skyFill * skyFill) * iblStrength;

        // Dampen by SSR confidence — the composite step adds ssrRefl*F*conf
        // back in, so the final specular = iblSpec*(1-conf) + ssrRefl*F*conf.
        // 1-frame latent trace (runs after this Lighting in each frame, so
        // we read the PREVIOUS frame's result) — the stale data is hidden by
        // the fact that SSR output moves slowly at screen-space velocity.
        const float ssrConf = gSSRResult.Load(int3(int2(i.pos.xy), 0)).a;
        specRadiance *= (1.0 - saturate(ssrConf));

        float3 Fibl    = FresnelSchlickRoughness(NdotV, F0, roughness);
        float2 envBRDF = gBRDFLUT.Sample(gIBLSampler, float2(NdotV, roughness));
        float3 specIBL = specRadiance * (Fibl * envBRDF.x + envBRDF.y);

        float3 kDibl     = (1.0 - Fibl) * (1.0 - metalness);
        // Diffuse IBL: AO + per-source scale + iblStrength on sky portion are
        // all already baked into iblDiffuse (split DDGI/Sky paths above).
        // Specular IBL: iblStrength is already applied per-source (sky portion
        // only) above; probes are independent. AO occludes all indirect specular.
        float3 diffIBL   = kDibl * albedo * iblDiffuse;
        float3 specIBLAO = specIBL * ao;

        iblContrib = diffIBL + specIBLAO;

#if NPR_PASS
        // IBL diffuse kept as a VECTOR (preserves environment color). Apply to
        // albedo directly at composite time, bypassing the ramp — ambient light
        // is omnidirectional so it shouldn't be shaded by NdotL ramp anyway.
        // iblStrength is already baked into iblDiffuse (sky portion) and
        // specIBLAO (sky/probe specular).
        nprAmbientColor  += kDibl * iblDiffuse;
        nprSpecularAccum += specIBLAO;
#endif
    }

    // Emissive is NOT in this output — it was written directly to
    // HdrSceneColor (RT5) by GBufferPass (Unreal-style). The PSO's additive
    // blend combines this lit result with that emissive seed, producing the
    // final SceneColor = lighting + emissive without emissive ever passing
    // through the BRDF. RT4 (gExtraGBuffer) is reserved for shading-model
    // scratch (SSS thickness, clearcoat data, …).
    float3 color = Lo + iblContrib;

    // ---- Subsurface Scattering (Phase F) -----------------------------------
    // Simple wrap-lit diffuse lobe tinted by a per-material SubsurfaceColor.
    // Convention: custom GBuffer PSes for Subsurface materials declare their
    // SSS parameters in this order (matches MaterialGPUData.customParams[N]
    // slot indices populated by Renderer::WriteMatSlot):
    //   customParams[0].rgb = SubsurfaceColor
    //   customParams[1].x   = WrapAmount       (0-1)
    //   customParams[2].x   = ScatterStrength  (0-1)
    if (runtimeShadingModel == SHADING_MODEL_SUBSURFACE)
    {
        MaterialGPUData sssMat     = gMaterials[matIdx];
        float3          sssColor   = sssMat.customParams[0].rgb;
        float           wrapAmt    = saturate(sssMat.customParams[1].x);
        float           scatterAmt = saturate(sssMat.customParams[2].x);

        // Per-pixel thickness from the "extra" GBuffer slot (RT4.r). The
        // custom GBuffer PS writes `saturate(thicknessMap.r)` here — zero
        // means "no scatter" (back-face / no thickness map assigned), one
        // means "light travels a lot through here" (flesh / wax / leaves).
        float thickness = gExtraGBuffer.Sample(gSampler, i.uv).r;

        // Wrap N·L past the terminator so light "seeps" further around.
        float NdotLWrap = saturate((dot(N, L) + wrapAmt) / (1.0 + wrapAmt));
        // Scatter contribution — wrap-lit albedo tinted by SSS color,
        // modulated by per-pixel thickness so thin areas stay lit normally.
        float3 scatter = albedo * sssColor * lightColor * NdotLWrap * shadowFactor;
        color += scatter * scatterAmt * thickness;
    }

#if NPR_PASS
    // ─── NPR post-ramp: stylize the PBR result ──────────────────────────
    // Read per-material NPR params from MaterialBuffer (indexed by GBuffer normal.a).
    MaterialGPUData nprMat = gMaterials[matIdx];
    uint  nprShaderType      = nprMat.shaderType;
    float nprShadowThreshold = nprMat.nprShadowThreshold;
    float nprShadowSmooth    = max(nprMat.nprShadowSmooth, 0.001);
    float nprRimPower        = max(nprMat.nprRimPower, 0.1);
    float nprRimStrength     = nprMat.nprRimStrength;
    float nprRampBlend       = nprMat.nprRampBlend;
    float nprBrightnessClamp = nprMat.nprBrightnessClamp;

    // Step 1: rampU from half-lambert × AO × CSM shadowFactor.
    // Including shadowFactor here makes characters standing in a shadow area
    // actually sample the darker ramp rows (fixes bright-face-in-shadow bug).
    float halfLambert = NdotL * 0.5 + 0.5;
    float rampU = saturate(halfLambert * lerp(1.0, ao, 0.9) * shadowFactor);
    // Apply shadow threshold + smooth step for stylized shadow edge
    rampU = smoothstep(nprShadowThreshold - nprShadowSmooth,
                       nprShadowThreshold + nprShadowSmooth, rampU);

    float3 rampColor;

    if (nprShaderType == SHADER_NPR_COLOR)
    {
        // ── NPR_COLOR: two-color editable ramp (no ramp texture required). ──
        float3 diffuseRamp = nprMat.nprDiffuseRampColor;
        float3 shadowRamp  = nprMat.nprShadowRampColor;

        // Edge soften — small rampU-driven smooth blend between shadow and lit colors.
        rampColor = lerp(shadowRamp, diffuseRamp, rampU);
    }
    else
    {
        // ── NPR_RAMP: sample multi-layer texture ramp (original skin-ramp path). ──
        float3 baseRamp = SampleRamp(rampU, NPR_ROW_BASE_SKIN);
        float3 midRamp  = SampleRamp(rampU, NPR_ROW_MID_SKIN);
        float3 veinRamp = SampleRamp(rampU, NPR_ROW_SHADOW_VEIN);
        float3 sssRamp  = SampleRamp(rampU, NPR_ROW_SSS);

        // Per-material skin-layer blend weights.
        float nprMidWeight  = nprMat.nprMidWeight;
        float nprVeinWeight = nprMat.nprVeinWeight;
        float nprSSSWeight  = nprMat.nprSSSWeight;

        float shadowMask = saturate(1.0 - rampU * 2.0);
        rampColor = baseRamp;
        rampColor = lerp(rampColor, midRamp,  nprMidWeight);
        rampColor = lerp(rampColor, veinRamp, shadowMask * ao * nprVeinWeight);
        rampColor = lerp(rampColor, sssRamp,  nprSSSWeight);
    }

    // ── Universal highlight recovery ───────────────────────────────────────
    // In strongly-lit areas, fade rampColor toward neutral white so the lit
    // side matches PBR (albedo × light) instead of stacking (albedo × warm-ramp),
    // which was causing skin to look noticeably redder than PBR under direct light.
    // Applied to both NPR_RAMP and NPR_COLOR.
    {
        float highlightFade = smoothstep(0.35, 0.9, rampU);
        rampColor = lerp(rampColor, float3(1, 1, 1), highlightFade);
    }

    // Ramp blend: 0 = neutral white (pure albedo tint, matches PBR),
    //             1 = full stylized ramp tint on the shadow side.
    rampColor = lerp(float3(1, 1, 1), rampColor, nprRampBlend);

    // Step 4: rebuild diffuse —
    //   direct lights: albedo × rampColor × nprLightEnergy (stylized via ramp).
    //   ambient IBL  : albedo × nprAmbientColor (full environment color,
    //                  NOT tinted by rampColor — so cool IBL still cools skin
    //                  exactly like PBR).
    float lightE = max(nprLightEnergy, 1e-5);
    float3 nprDiffuse = albedo * rampColor * lightE
                      + albedo * nprAmbientColor;

    // Step 5: final composite — specular stays PBR. Emissive arrives via
    // additive blend from the GBuffer-seeded HdrSceneColor (Unreal-style
    // direct emissive write) — no additive term here.
    color = nprDiffuse + nprSpecularAccum;

    // Step 6: rim light — per-material rimPower + rimStrength.
    // NPR_COLOR uses the diffuse-ramp color as rim tint; NPR_RAMP keeps the texture-driven rim.
    // Rim is physically a back-light effect, so suppress it on the lit side:
    // (1 - NdotL) fades rim out where the surface faces the light, and
    // shadowFactor further hides rim on pixels inside CSM shadow.
    float3 rimColor = (nprShaderType == SHADER_NPR_COLOR)
                    ? nprMat.nprDiffuseRampColor
                    : NPRRimColor(NdotV);
    float  rimFacing = saturate(NdotL);            // 1 when fully facing light
    float  rimMask   = pow(saturate(1.0 - NdotV), nprRimPower)
                     * (1.0 - rimFacing)
                     * shadowFactor;
    color += albedo * rimColor * rimMask * nprRimStrength;

    // Step 7: min brightness floor — max of cbuffer global floor and per-material setting.
    // Both default to 0 so NPR shadows can reach true black (matches PBR), which
    // keeps the AutoExposure histogram aligned with the PBR case and prevents
    // "strong-lit skin looks redder than PBR under ACES" artefacts.
    // Artists can raise either value for a stylized soft-shadow floor.
    float minBrightness = max(nprMat.nprMinBrightness, nprMinBrightness);
    if (minBrightness > 0.0)
        color = max(color, albedo * minBrightness);

    // Step 8: unified brightness ceiling — combine the two clamps into a single
    // Luminance() pass. Whichever limit is tighter wins.
    //   nprBrightnessClamp > 0: relative cap = baseColorLum * nprBrightnessClamp
    //   nprMaxBrightness   > 0: absolute HDR luminance cap
    {
        float nprMaxBrightness = nprMat.nprMaxBrightness;
        const float kNoLimit = 3.402823466e+38f; // FLT_MAX
        float maxLum = kNoLimit;

        if (nprBrightnessClamp > 0.0)
        {
            float baseColorLum = max(Luminance(albedo), 1e-5);
            maxLum = min(maxLum, baseColorLum * nprBrightnessClamp);
        }
        if (nprMaxBrightness > 0.0)
            maxLum = min(maxLum, nprMaxBrightness);

        if (maxLum < kNoLimit)
        {
            float curLum = Luminance(color);
            if (curLum > maxLum)
                color *= maxLum / curLum;
        }
    }
#endif

    // ---- Aerial Perspective composite ---------------------------------------
    // Apply atmospheric scattering + transmittance based on view distance.
    // LAST (after the NPR post-ramp): distance fog is a view-medium effect —
    // it applies regardless of shading model, and the NPR brightness clamps
    // must not crush it (the old placement before NPR_PASS was overwritten by
    // `color = nprDiffuse + ...` on NPR pixels).
    // Distance → 3D LUT slice uses the same quadratic mapping the baking
    // shader uses:  slice_t = sqrt(distKm / maxDistKm).  Capped so very far
    // points clamp to the last slice instead of mapping to uninitialised data.
    if (aerialMaxDistKm > 0.0)
    {
        float distKm = length(worldPos - cameraPos) * 0.001; // metres → km
        distKm = min(distKm, aerialMaxDistKm);               // clamp far range
        float sliceT = saturate(sqrt(distKm / aerialMaxDistKm));
        float4 ap = gAerialPerspective.SampleLevel(gSampler, float3(i.uv, sliceT), 0.0);

        // Defensive guard: if the AP LUT hasn't been written yet (all-zero
        // sample) the shader would multiply the scene by ap.a = 0 and paint
        // everything black. Treat that sample as a no-op — `color` passes
        // through unchanged until the LUT dispatch produces valid data.
        if (ap.a > 1e-4 || any(ap.rgb > 1e-4))
            color = color * ap.a + ap.rgb;
    }

    return float4(color, 1.0);
#endif // UNLIT
}

