// FroxelLightInject.cs.hlsl
// -----------------------------------------------------------------------------
// Pass 3 — multiplies the per-froxel scattering coefficient by the incoming
// light at that point. Output:
//   .rgb = density.scattering × Σ(L_in × phase × shadow)
//   .a   = density.extinction   (carried through unchanged)
//
// Currently injects:
//   - Directional sun light, shadowed by the existing CSM array (gShadowCascades
//     at t9 space0 — kept binding-compatible with the deferred lighting pass).
//   - Constant ambient term (so dark voxels still pick up a small grey).
//
// Spot / point light injection can be added later by binding the existing
// GPULight + cluster grid SRVs in the same way LightingPass does.
// -----------------------------------------------------------------------------

#include "FroxelCommon.hlsli"

#include "cluster_common.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture3D<float4>           SrcDensity     : register(t0, space2);
Texture2DArray<float>       ShadowCascades : register(t1, space2);
StructuredBuffer<GPULight>  Lights         : register(t2, space2);
// Scene depth buffer (camera view) — used for cheap screen-space shadow
// raymarching between each voxel and the spot/point light it's receiving
// from. Without this every spot light beams straight through walls.
Texture2D<float>            SceneDepth     : register(t3, space2);
// World-space occupancy grid built by SceneVoxelPass. 128³ R8_UINT; non-zero
// means "some scene AABB intersects this voxel". Complements SceneDepth by
// catching occluders that are off-screen (where screen-space shadow fails).
Texture3D<uint>             SceneOccupancy : register(t4, space2);
// Per-spot-light shadow atlas (opt-in via LightData::castsShadow).
// Layout mirrors the graphics root sig's t21/t22 space0 slots.
Texture2DArray<float>       SpotShadowAtlas : register(t6, space2);
StructuredBuffer<float4x4>  SpotShadowVPs   : register(t7, space2);
// NOTE: s0 space2 is a regular linear sampler (shared across all compute
// passes). The CSM PCF lookup needs a COMPARISON sampler — declared as s1.
SamplerComparisonState      ShadowSampler  : register(s1, space2);
SamplerState                LinearSampler  : register(s0, space2);

RWTexture3D<float4>         DstLighting    : register(u0, space2);

// ---------------------------------------------------------------------------
// Screen-space shadow — march from voxel toward light, project each sample
// into the camera's view, compare its NDC z against the scene depth buffer.
// Points whose projected z places them behind the nearest scene surface
// (under reversed Z: ndc.z < sceneDepth) count as occluded. Returns [0, 1].
//
// Caveats:
//   * Catches only occluders that are *visible to the camera*. Geometry just
//     off-screen (or occluders hidden by other occluders) slips through.
//   * Cheaper than a per-light shadow map — ~kSteps depth taps per voxel,
//     per in-range light. Suitable as a first-pass occlusion term.
// ---------------------------------------------------------------------------
float ScreenSpaceShadow(float3 fromWorld, float3 toWorld)
{
    const int kSteps = 6;
    int   valid    = 0;
    float occluded = 0.0;

    [unroll] for (int i = 1; i <= kSteps; ++i)
    {
        // t in (0,1) — skip endpoints (the voxel itself and the light apex)
        float  t        = float(i) / float(kSteps + 1);
        float3 samplePos = lerp(fromWorld, toWorld, t);

        float4 clip = mul(float4(samplePos, 1.0), P.viewProj);
        if (clip.w <= 0.0) continue; // behind camera — can't test

        float3 ndc = clip.xyz / clip.w;
        float2 uv  = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

        float sceneZ = SceneDepth.SampleLevel(LinearSampler, uv, 0).r;
        // Reversed Z: larger ndc.z = closer. Sample is behind geometry when
        // its ndc.z is smaller than the scene's depth at that UV.
        if (ndc.z < sceneZ - 1e-4) occluded += 1.0;
        valid += 1;
    }

    if (valid == 0) return 1.0; // entire ray off-screen → assume visible
    return saturate(1.0 - occluded / float(valid));
}

// ---------------------------------------------------------------------------
// World-space voxel occlusion — complements ScreenSpaceShadow by catching
// occluders that lie outside the camera's view (off-screen walls, objects
// hidden behind other objects in the depth buffer, etc.).
//
// Walks the world-space ray from voxel → light at HALF-VOXEL step size so a
// single-voxel-thick wall can't slip between samples, and uses hard-shadow
// logic — any hit blocks the ray entirely. Averaging a single hit across 16
// samples dilutes it to ~6 % dimming, which is invisible; binary occlusion
// matches real-world "wall blocks light" behaviour.
//
// Returns 1 when the grid is not bound (voxelGridDim == 0 or any extent == 0)
// so the caller falls back to ScreenSpaceShadow-only behaviour.
// ---------------------------------------------------------------------------
float VoxelOcclusion(float3 fromWorld, float3 toWorld)
{
    if (P.voxelGridDim == 0u) return 1.0;
    if (any(P.voxelGridExtent <= 0.0)) return 1.0;

    float3 dir  = toWorld - fromWorld;
    float  dist = length(dir);
    if (dist < 1e-4) return 1.0;
    dir /= dist;

    // Assume ~cubic voxels (SceneVoxelPass builds the grid that way); extent.x
    // / dim is the voxel edge length. Half-voxel step = no skipping.
    float voxelSize = P.voxelGridExtent.x / float(P.voxelGridDim);
    float stepSize  = max(voxelSize * 0.5, 0.01);
    // 96-step cap = 48 voxels max (~24 m at 0.5 m/voxel). Rays longer than
    // that are rare in practice and would be too costly to march fully.
    int   maxSteps  = min(int(dist / stepSize), 96);

    float3 invExtent = 1.0 / P.voxelGridExtent;

    // i=1 skips the voxel's own cell (avoids self-occlusion when the fog
    // voxel happens to lie inside a marked AABB). We also stop one step
    // short of the light so the light's own marked cell (if any) doesn't
    // swallow itself — though Renderer already filters light entities out
    // of the occupancy AABB list as belt-and-braces.
    [loop] for (int i = 1; i < maxSteps - 1; ++i)
    {
        float3 samplePos = fromWorld + dir * (float(i) * stepSize);
        float3 uvw       = (samplePos - P.voxelGridMin) * invExtent;
        if (any(uvw < 0.0) || any(uvw > 1.0)) continue;

        int3 vc = clamp(int3(uvw * float(P.voxelGridDim)),
                        0, int(P.voxelGridDim) - 1);
        if (SceneOccupancy.Load(int4(vc, 0)) > 0u)
            return 0.0; // hard shadow: single hit fully blocks the ray
    }

    return 1.0;
}

// ---------------------------------------------------------------------------
// Shadow-map visibility for a spot light that opted in (shadowSliceIdx valid).
// Supersedes the screen-space + voxel approximations — one SampleCmp tap
// returns exact occlusion for the slice's authored frustum.
// ---------------------------------------------------------------------------
float SpotShadowMapVisibility(float3 worldPos, uint sliceIdx)
{
    float4 sc = mul(float4(worldPos, 1.0), SpotShadowVPs[sliceIdx]);
    // Behind light's projection plane (opposite hemisphere) — treat as occluded.
    // The shadow frustum matches the spot cone, so geometry behind the light
    // apex can only reach a voxel by passing through the light itself; dark.
    if (sc.w <= 0.0) return 0.0;
    sc.xyz /= sc.w;
    float2 uv = sc.xy * float2(0.5, -0.5) + 0.5;
    // UV / depth outside the shadow frustum means the voxel is outside the
    // spot's cone coverage. Returning 1.0 here let light "escape" the cone
    // at the far / side boundaries (the bottom-leak artifact). Force occluded
    // so the authored shadow frustum hard-bounds the volumetric contribution.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        sc.z < 0.0 || sc.z > 1.0)
        return 0.0;

    // Receiver-side bias is now tiny — the rasterizer's slope-scaled DepthBias
    // handles the bulk of shadow acne during caster rendering, and this
    // residual term just covers depth quantisation on the receiver side. The
    // previous 0.0015 was pushing receivers so far toward the light that
    // pillar-base voxels slipped above thin occluders and lit through them.
    const float bias = 0.00025;

    // 2×2 PCF — spreads visibility across the froxel's shadow-map footprint.
    // A single tap at the voxel centre misses pillars thinner than one texel
    // (the texel averages a mix of "on pillar" and "on floor" occluders, but
    // the single lookup lands on whichever fragment wrote last). Averaging
    // four offset taps turns the hard per-texel threshold into a smooth
    // 0→1 ramp at geometry edges, which is what the temporal pass needs to
    // converge to clean shadow edges around pillar bases.
    //
    // Atlas slices are 1024² (see SpotShadowPass::kShadowMapSize) — texel
    // size is hard-coded here to avoid plumbing a new CB field. Keep in sync
    // if the atlas size changes.
    const float kTexelSize = 1.0 / 1024.0;
    float visibility = 0.0;
    [unroll] for (int dy = 0; dy <= 1; ++dy)
    [unroll] for (int dx = 0; dx <= 1; ++dx)
    {
        float2 o = (float2(dx, dy) - 0.5) * kTexelSize;
        visibility += SpotShadowAtlas.SampleCmpLevelZero(
            ShadowSampler, float3(uv + o, (float)sliceIdx), sc.z + bias);
    }
    return visibility * 0.25;
}

// Spot-light count is packed into shadowParams.y (was the unused
// blendRange field in the v1 layout). Keeping the same FroxelParams CB
// avoids a root-sig touch.
#define SPOT_LIGHT_COUNT  ((uint)(P.shadowParams.y))

// ---------------------------------------------------------------------------
// CSM PCF — now that ShadowPass leaves the array in DEPTH_READ_SRV (which
// includes NON_PIXEL_SHADER_RESOURCE) and the compute root sig has a
// dedicated comparison sampler at s1 space2, the volumetric pass can read
// CSM directly.
//
// IMPORTANT: instead of selecting a cascade via view-Z (which depended on a
// correctly-propagated cameraForward + cascadeSplits and silently fell back
// to "no shadow" when those were wrong) we just probe each cascade in
// near→far order and use the first one whose projected UV lies inside the
// shadow-map bounds. This is bulletproof against bad CB data and matches
// what the surface shader effectively sees.
// ---------------------------------------------------------------------------
float SampleSingleCascade(int cascade, float4x4 m, float3 worldPos)
{
    float4 sp = mul(float4(worldPos, 1.0), m);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;

    // Out-of-cascade signal — caller falls through to the next cascade.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        sp.z < 0.0 || sp.z > 1.0)
        return -1.0;

    // Reversed-Z: closer surfaces map to LARGER depth values. Self-shadowing
    // happens when the receiver's reconstructed z is fractionally BELOW the
    // SM's recorded z, so we push the receiver slightly higher (closer) to
    // bias the SampleCmp GREATER_EQUAL compare toward "lit".
    float depth     = sp.z + P.shadowParams.z;
    float texelSize = P.shadowParams.x;

    float shadow = 0.0;
    [unroll] for (int dy = 0; dy <= 1; ++dy)
    [unroll] for (int dx = 0; dx <= 1; ++dx)
    {
        float2 o = (float2(dx, dy) - 0.5) * texelSize;
        shadow += ShadowCascades.SampleCmpLevelZero(
            ShadowSampler, float3(uv + o, float(cascade)), depth);
    }
    return shadow * 0.25;
}

// Select the cascade via "first in-bounds UV wins" (near → far). The surface
// shader can pick by view-Z because its view-Z is recovered directly from the
// depth buffer and is exact. Our voxel view-Z is reconstructed from the log
// slice distribution, so near cascade boundaries the selection is often wrong
// — and picking cascade 0 for a far voxel ends up sampling the cleared
// (1.0) portion of cascade 0's shadow map → the voxel looks fully lit, which
// is exactly the frustum-shaped bright/dark banding users saw. Probing each
// cascade in order and taking the first one whose UV is inside the SM bounds
// is bulletproof against that CB drift.
float ComputeSunShadow(float3 worldPos)
{
    float4x4 mats[3] = { P.shadowMatrix0, P.shadowMatrix1, P.shadowMatrix2 };
    [unroll] for (int c = 0; c < 3; ++c)
    {
        float s = SampleSingleCascade(c, mats[c], worldPos);
        if (s >= 0.0) return s;
    }
    return 1.0;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH || DTid.z >= P.froxelD)
        return;

    // Temporal sub-voxel jitter — moves the sample point each frame so the
    // hard discontinuities (spot cone edge, CSM boundary) are sampled at
    // slightly different world positions and the temporal reprojection pass
    // can smooth them into continuous gradients.
    // XY-only sub-voxel jitter. The Z axis is intentionally NOT jittered —
    // froxels at the far end are metres wide in world-space, and a ±0.25-slice
    // Z shift per frame shows up as noticeable depth flicker that temporal
    // reprojection can't hide.
    float3 jitter = GetFroxelJitter(P.frameIndex);
    jitter.z = 0.0;
    float3 worldPos = FroxelToWorldJittered(DTid, P, jitter);
    float4 dens     = SrcDensity[DTid];

    // View direction at this voxel (camera → froxel).
    float3 V = normalize(worldPos - P.cameraPos);

    // ---- Directional sun & shadow-casting lights -----------------------------
    // Handled by the per-pixel VolumetricRaymarch pass, not this froxel grid.
    // The grid is too coarse (128 Z slices) to resolve sharp CSM cascade edges
    // or spot-atlas shadow boundaries; raymarching at full-res reads them
    // exactly. This pass now only injects the "cheap many-lights" tier:
    // opted-in lights without shadow maps.
    float3 sunRadiance = float3(0, 0, 0);

    // ---- Spot / point lights (no-shadow tier only) --------------------------
    float3 spotRadiance = float3(0, 0, 0);
    uint   spotCount    = SPOT_LIGHT_COUNT;
    [loop] for (uint li = 0; li < spotCount; ++li)
    {
        GPULight L = Lights[li];
        if (L.type == 0u) continue;                 // skip directional
        // Skip lights that have a shadow-atlas slice — those are handled by
        // the VolumetricRaymarch pass (per-pixel raymarch + shadow-map lookup)
        // for high-quality light shafts with sharp occluders.
        if (L.shadowSliceIdx != 0xFFFFFFFFu) continue;

        float3 toL    = L.position - worldPos;
        float  dist   = length(toL);
        if (dist > L.radius) continue;
        float3 Ldir   = toL / max(dist, 1e-4);

        // Distance attenuation for VOLUMETRIC fog. Unlike surface lighting
        // (which uses 1/d² to match solid-angle falloff) the volumetric path
        // must avoid 1/d² entirely — near the light source the froxel voxel
        // contains the light position itself, jitter shifts the sample point
        // slightly inside/outside the singularity, and the inverse square
        // amplifies that into a visible per-frame flicker at the cone apex.
        //
        // Use a smooth windowed falloff (Frostbite style) that approaches 1
        // near the light, tapers to 0 at radius edge, and is numerically
        // stable everywhere. The density / transmittance integration already
        // provides the physical brightness dropoff along the ray.
        float distNorm  = dist / max(L.radius, 1e-4);
        float distNorm2 = distNorm * distNorm;
        float atten     = saturate(1.0 - distNorm2 * distNorm2);
        atten *= atten;

        // Spot cone falloff. The smoothstep transition starts at 0 (the light
        // direction axis) and ends at cosOuter (cone edge). This gives the
        // widest possible soft edge so the limited froxel grid can always
        // resolve the cone boundary without visible rings — even a narrow
        // beam gets a silky gradient. If you want sharper edges, raise the
        // "inner" start value toward cosOuter.
        float spotFactor = 1.0;
        if (L.type == 2u && L.spotAngle > 0.0)
        {
            // Soft-edge band. Width is a trade-off: too narrow → not enough
            // froxels span the ramp and the edge aliases into stair-steps;
            // too wide → the cone looks mushy with no visible boundary.
            // At typical spot angles (15°–40°) a band covering the outer
            // ~30 % of the cone works out to 3–5 voxels of ramp at common
            // ranges, which is enough for temporal + full-voxel jitter to
            // resolve smoothly without blurring the whole cone. Adjust the
            // 0.70 factor toward cosOuter (i.e. closer to 1.0) for crisper
            // edges, or away (e.g. 0.5) for softer ones.
            float cosA     = dot(-Ldir, normalize(L.direction));
            float cosOuter = cos(L.spotAngle);
            float cosInner = cos(L.spotAngle * 0.70);
            spotFactor = smoothstep(cosOuter, cosInner, cosA);
        }
        if (spotFactor <= 0.0) continue;

        // Phase function — view towards LIGHT (Ldir is from voxel to light).
        float spotCos   = dot(V, Ldir);
        float spotPhase = PhaseHG(spotCos, P.anisotropy);

        // No shadow-atlas for these lights (tier 4 in the taxonomy: many cheap
        // lights). We still use the screen-space + voxel-grid approximations
        // so spot beams don't leak through walls they happen to hit. For the
        // "strictly no occlusion" tier, drop these two calls.
        float visSS = ScreenSpaceShadow(worldPos, L.position);
        float visVX = VoxelOcclusion   (worldPos, L.position);
        float visibility = min(visSS, visVX);

        spotRadiance += L.color * L.intensity * atten * spotFactor * spotPhase * visibility;
    }

    // ---- Ambient (cheap "multi-scatter" stand-in) ----
    float3 ambient = P.ambientColor * P.ambientContribution / (4.0 * FROXEL_PI);

    float3 inLight = sunRadiance + spotRadiance + ambient;

    // Final per-voxel emitted radiance:  σ_s × L_in    (extinction passes through)
    DstLighting[DTid] = float4(dens.rgb * inLight, dens.a);
}
