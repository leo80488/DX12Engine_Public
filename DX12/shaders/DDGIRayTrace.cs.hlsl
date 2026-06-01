// DDGIRayTrace.cs.hlsl — DDGI probe ray trace, compute-shader edition.
//
// Rewritten 2026-05 (was DDGIRayTrace.lib.hlsl, an RTPSO with raygen / miss /
// closesthit shaders). Now uses inline RayQuery (DXR 1.1, SM 6.5+) so the
// engine no longer needs an RTPSO + shader-binding-table for DDGI; everything
// lives in a single CS.
//
// Dispatch: numthreads(32,1,1) — Dispatch(ceil(raysPerProbe/32), probeCount, 1).
//   DTid.x = rayIdx within probe (early-out if >= raysPerProbe)
//   DTid.y = probeIdx
//
// Output: g_RayData[probeIdx * raysPerProbe + rayIdx] = float4(radiance, dist).
// Identical layout to the old RTPSO output so the relight CS is unchanged.
//
// Shading: per-instance albedo + smooth-shaded normal (bindless VB/IB lookup) +
// shadow ray to the sun (NdotL × visibility). The closest-hit logic from the
// old library is folded inline.
//
// Multi-bounce (Task #2): when frameIndex > 0 the closest-hit also samples
// previous-frame DDGI via the bound irradiance/depth/probeData of THIS volume,
// scaled by 0.95 / PI for energy conservation.

#include "DDGICommon.hlsli"
#include "DDGISampling.hlsli"
#include "cluster_common.hlsli"  // GPULight

// ---- Bindings -------------------------------------------------------------
// Same root sig as before — see DDGIPass.h:
//   b0 space0 — DDGIVolumeGPU
//   t0 space0 — TLAS
//   t1 space0 — sky IBL cube
//   u0 space0 — RayData (output)
//   t3 space0 — per-instance data
//   t4 space0 — irradiance atlas SRV   (multi-bounce read)
//   t5 space0 — depth atlas SRV        (multi-bounce read)
//   t6 space0 — probe data SRV         (multi-bounce read)
//   t0 space1 — bindless ByteAddressBuffer table (16384 slots)
//   s0 space0 — linear sampler

ConstantBuffer<DDGIVolumeGPU>      g_Vol      : register(b0, space0);
RaytracingAccelerationStructure    g_TLAS     : register(t0, space0);
TextureCube<float4>                g_SkyIBL   : register(t1, space0);
RWStructuredBuffer<float4>         g_RayData  : register(u0, space0);

struct DDGIInstanceData
{
    float4 baseColor;
    float4 emissive;       // rgb pre-multiplied by strength; a unused
    uint   vbBindlessIdx;
    uint   ibBindlessIdx;
    uint   vertexStart;
    uint   indexStart;
    int    emissiveTexIdx; // bindless tex pool index, -1 = none
    uint   _pad0, _pad1, _pad2;
};
StructuredBuffer<DDGIInstanceData> g_DDGIInstances : register(t3, space0);

// Multi-bounce: previous-frame DDGI. Sampled on hit when g_Vol.frameIndex > 0.
StructuredBuffer<DDGIProbeSH>      g_PrevProbeSH    : register(t4, space0);
Texture2D<float2>                  g_PrevDepth      : register(t5, space0);
StructuredBuffer<DDGIProbeData>    g_PrevProbeData  : register(t6, space0);

// Cluster GPULight buffer — same buffer the rasterizer LightingPass reads.
// Trace CS picks a random light per ray and importance-samples by lightCount.
StructuredBuffer<GPULight>         g_Lights         : register(t7, space0);

// Adaptive ray dispatch — descriptors prepared by DDGIRayAllocation.cs.
// g_RayAlloc[0..2] = dispatch args (read by ExecuteIndirect, not by us).
// g_RayAlloc[3]    = total rays this frame.
// g_RayAlloc[4..]  = packed (probeIdx & 0xFFFFF) | (rayIdx << 20).
RWStructuredBuffer<uint>           g_RayCount       : register(u2, space0);
RWStructuredBuffer<uint>           g_RayAlloc       : register(u3, space0);

ByteAddressBuffer g_DDGIBuffers[16384] : register(t0, space1);

// Engine-wide bindless texture table (matches GraphicsDX12::kMaxBindlessTextures
// = 16384). Used by closest-hit to sample emissive textures so glowing surfaces
// (lamps, neon signs, screens, etc.) light up the SH probes via DDGI.
Texture2D                          g_AllTextures[16384] : register(t0, space2);

SamplerState                       g_LinearSampler : register(s0, space0);

#define DDGI_VERTEX_STRIDE_BYTES 32
#define DDGI_INDEX_BYTES         4

// ---- Helpers --------------------------------------------------------------
// Same bindless triangle attribute fetch as the old closest-hit. Returns
// position + normal + UV per triangle vertex. When the instance lacks
// bindless registration we fall back to defaults.
bool LoadHitTriangleAttributes(uint instId, uint primIdx,
                               out float3 p0, out float3 p1, out float3 p2,
                               out float3 n0, out float3 n1, out float3 n2,
                               out float2 uv0, out float2 uv1, out float2 uv2)
{
    DDGIInstanceData inst = g_DDGIInstances[instId];
    p0 = p1 = p2 = float3(0, 0, 0);
    n0 = n1 = n2 = float3(0, 1, 0);
    uv0 = uv1 = uv2 = float2(0, 0);
    if (inst.vbBindlessIdx == 0xFFFFFFFFu || inst.ibBindlessIdx == 0xFFFFFFFFu)
        return false;

    const uint vbIdx = NonUniformResourceIndex(inst.vbBindlessIdx);
    const uint ibIdx = NonUniformResourceIndex(inst.ibBindlessIdx);

    uint i0Off = (inst.indexStart + primIdx * 3 + 0) * DDGI_INDEX_BYTES;
    uint i1Off = (inst.indexStart + primIdx * 3 + 1) * DDGI_INDEX_BYTES;
    uint i2Off = (inst.indexStart + primIdx * 3 + 2) * DDGI_INDEX_BYTES;
    uint i0 = g_DDGIBuffers[ibIdx].Load(i0Off);
    uint i1 = g_DDGIBuffers[ibIdx].Load(i1Off);
    uint i2 = g_DDGIBuffers[ibIdx].Load(i2Off);

    uint v0Off = i0 * DDGI_VERTEX_STRIDE_BYTES;
    uint v1Off = i1 * DDGI_VERTEX_STRIDE_BYTES;
    uint v2Off = i2 * DDGI_VERTEX_STRIDE_BYTES;
    p0 = asfloat(g_DDGIBuffers[vbIdx].Load3(v0Off + 0));
    p1 = asfloat(g_DDGIBuffers[vbIdx].Load3(v1Off + 0));
    p2 = asfloat(g_DDGIBuffers[vbIdx].Load3(v2Off + 0));
    n0 = asfloat(g_DDGIBuffers[vbIdx].Load3(v0Off + 12));
    n1 = asfloat(g_DDGIBuffers[vbIdx].Load3(v1Off + 12));
    n2 = asfloat(g_DDGIBuffers[vbIdx].Load3(v2Off + 12));
    // UV at offset 24 (after pos[0..11] + nrm[12..23]).
    uv0 = asfloat(g_DDGIBuffers[vbIdx].Load2(v0Off + 24));
    uv1 = asfloat(g_DDGIBuffers[vbIdx].Load2(v1Off + 24));
    uv2 = asfloat(g_DDGIBuffers[vbIdx].Load2(v2Off + 24));
    return true;
}

// Inline shadow ray with parameterised range. Returns 1 = light visible, 0 = blocked.
float ShadowVisibility(float3 origin, float3 dir, float maxDist)
{
    RayDesc r;
    r.Origin    = origin + dir * 0.05; // bias to avoid self-hit
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = maxDist;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
           | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// PCG-style hash for per-ray light pick. Cheap and uncorrelated across the
// ray grid + frames. Initialised once per closesthit; mixed from probe/ray
// indices and the volume's frameIndex.
uint HashPCG(uint v)
{
    v = v * 747796405u + 2891336453u;
    uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}

// Evaluate one cluster GPULight against a surface point. Returns the radiant
// flux arriving at P along its incoming direction (without the Lambertian
// 1/PI factor — caller folds that in along with albedo).
float3 EvalLight(GPULight light, float3 P, float3 N)
{
    float3 lightCol = light.color * light.intensity;
    float3 L        = float3(0, 0, 0);
    float  dist     = 0.0;

    if (light.type == 0u)
    {
        // Directional — direction stored is from-light (same convention as
        // LightCB.lightDir). Negate to get the surface-to-light direction.
        L    = -normalize(light.direction);
        // Shadow-ray TMax. The previous 1e6 made every directional shadow
        // ray on a sky-facing surface traverse the entire TLAS before
        // missing — at scale (≥2048 probes × 64 rays in a 3000+ instance
        // scene) that pushed the DDGI compute CL past Windows' 2-second
        // TDR threshold (DXGI_ERROR_DEVICE_HUNG). 100 is enough for any
        // typical scene; tighter than 500 (was capped earlier) to claw back
        // more headroom for dense TLAS scenes. Anything past 100 units is
        // effectively unoccluded for indirect-diffuse purposes anyway.
        dist = 100.0;
    }
    else if (light.type == 1u)
    {
        // Point — radius is the cutoff range.
        float3 d = light.position - P;
        dist = length(d);
        if (dist > light.radius) return float3(0, 0, 0);
        L = d / max(dist, 1e-4);
        float att = saturate(1.0 - dist / light.radius);
        att = att * att;
        lightCol *= att;
    }
    else
    {
        // Spot
        float3 d = light.position - P;
        dist = length(d);
        if (dist > light.radius) return float3(0, 0, 0);
        L = d / max(dist, 1e-4);
        float spot = dot(-L, normalize(light.direction));
        float ca   = cos(light.spotAngle);
        if (spot < ca) return float3(0, 0, 0);
        float spotFalloff = saturate((spot - ca) / max(1e-3, 1.0 - ca));
        float att         = saturate(1.0 - dist / light.radius);
        att = att * att * spotFalloff;
        lightCol *= att;
    }

    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return float3(0, 0, 0);

    float vis = ShadowVisibility(P, L, dist);
    return lightCol * NdotL * vis;
}

// ---- Entry ----------------------------------------------------------------
[numthreads(32, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    // Adaptive dispatch: a single 1D index over all rays this frame. Decode
    // the (probeIdx, rayIdx) pair from the packed descriptor. Direction is
    // reconstructed via Halton(2,3) at index `rayIdx` (prefix-uniform, so it
    // doesn't depend on this probe's per-frame ray count); the relight CS
    // reconstructs the same direction from the same index.
    const uint linearIdx  = DTid.x;
    const uint totalRays  = g_RayAlloc[0];
    if (linearIdx >= totalRays) return;

    const uint packed     = g_RayAlloc[1 + linearIdx];
    const uint probeIdx   = packed & 0xFFFFFu;
    const uint rayIdx     = packed >> 20u;
    // adaptiveRays is read for symmetry with the relight; the ray dispatch
    // already encoded only valid (probeIdx, rayIdx) pairs into g_RayAlloc.
    // Direction generation now uses Halton(2,3), which has the prefix-uniform
    // property — `i` maps to the same direction regardless of total count, so
    // the previous "pass raysPerProbe (max) to keep cosTh stable" workaround
    // is no longer needed. See DDGICommon.hlsli for details.
    const uint adaptiveRays  = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;

    // Probe origin — base grid + relocation offset from the previous frame's
    // probe relocation CS. Without the offset, probes that the relocation
    // pass pushed out of nearby walls would still TRACE from inside-wall
    // positions, producing degenerate close-range hits that re-trigger
    // relocation in a feedback loop. Sampling already uses pd.offset, so
    // matching the trace origin keeps the two ends consistent.
    int3   coord    = DDGI_ProbeCoord(probeIdx, g_Vol);
    float3 probePos = DDGI_ProbeWorldPos(coord, g_Vol);
    {
        DDGIProbeData pd = g_PrevProbeData[probeIdx];
        probePos += pd.offset;
    }

    // Direction = Halton(2,3) × per-frame random rotation. Adaptive sample
    // index spreads this probe's adaptiveRays rays across the volume's full
    // Halton sequence + rotates the subset per frame so EMA reaches the
    // whole sphere (see DDGICommon.hlsli for why naive prefix isn't enough).
    const uint sampleIdx = DDGI_AdaptiveSampleIdx(rayIdx, adaptiveRays, g_Vol);
    float3 dirLocal = DDGI_HaltonSphere(sampleIdx);
    float3 rayDir   = DDGI_ApplyRandomRotation(dirLocal, g_Vol);

    RayDesc ray;
    ray.Origin    = probePos;
    ray.Direction = rayDir;
    ray.TMin      = 0.05;
    // Trace TMax. The previous 200.0 was set when TLAS density was much
    // smaller; with the engine's current ~3657 BLAS instances each ray
    // worst-cases hundreds of intersection tests, and at 2048+ probes the
    // dispatch went over Windows' 2-second TDR threshold (DXGI_ERROR_DEVICE_HUNG
    // observed at exactly 16×8×16 = 2048 probes, while 16×7×16 = 1792 ran
    // fine — confirming this scales linearly with ray count).
    //
    // 50.0 keeps coverage well past any reasonable single-volume extent
    // while clamping the worst-case BLAS visits per ray. Beyond ~50 units
    // most indirect contribution is already attenuated through multiple
    // bounces, so the visual loss is small. Bump for very-large outdoor
    // scenes only after profiling rules out TDR.
    ray.TMax      = 50.0;

    // Backface culling INTENTIONALLY OFF — we need to SEE backface hits so we
    // can detect probes that are buried in solid geometry (or sitting on the
    // wrong side of a thin wall, the indoor↔outdoor leak vector). With cull
    // on, those rays pass straight through walls into the sky and pollute the
    // probe's SH with whatever's on the other side. With cull off, we get
    // CommittedTriangleFrontFace() = false for those hits and we can route
    // them to a separate "this direction is buried" path (zero radiance,
    // backface-sentinel hitDistance) read by Relight's depth atlas + Relocate's
    // probe-classification pass.
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    float3 radiance    = float3(0, 0, 0);
    float  hitDistance = -1.0;

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        // ---- Miss -----------------------------------------------------------
        float3 sky = g_SkyIBL.SampleLevel(g_LinearSampler, rayDir, 0).rgb;
        radiance    = min(sky, float3(10.0, 10.0, 10.0));
        hitDistance = -1.0;
    }
    else if (!q.CommittedTriangleFrontFace())
    {
        // ---- Backface hit ---------------------------------------------------
        // Probe is on the SOLID side of this surface along this ray direction.
        // Do NOT integrate "what's behind the wall" — that's exactly how
        // outdoor sky leaks into indoor probes through thin walls.
        //
        // Encoding contract (see DDGIRelight.cs / DDGIProbeRelocate.cs):
        //   hitDistance > 0       : frontface hit at this distance
        //   hitDistance == -1.0   : miss (no hit)
        //   hitDistance < -1.5    : backface hit, real t = -(hitDistance + 2.0)
        //
        // The -2.0 offset keeps backface values strictly below -1.5 so the
        // miss sentinel (-1.0) sits in an unambiguous gap, while the actual
        // distance to the wall is preserved for Chebyshev (depth atlas
        // records the wall as a near occluder).
        const float t = q.CommittedRayT();
        radiance    = float3(0, 0, 0);
        hitDistance = -(t + 2.0);
    }
    else
    {
        // ---- Frontface hit (folds the old DDGIClosestHit in-place) ---------
        const uint  instId    = q.CommittedInstanceID();
        const uint  primIdx   = q.CommittedPrimitiveIndex();
        const float t         = q.CommittedRayT();
        const float2 bary     = q.CommittedTriangleBarycentrics();
        const float3x4 obj2w  = q.CommittedObjectToWorld3x4();

        DDGIInstanceData inst = g_DDGIInstances[instId];
        float3 albedo   = inst.baseColor.rgb;
        float3 emissive = inst.emissive.rgb;

        float3 hitPos = ray.Origin + ray.Direction * t;

        // Smooth-shaded normal + interpolated UV via bindless triangle attribute fetch.
        float3 worldN;
        float2 hitUV = float2(0, 0);
        bool   hasUV = false;
        {
            float3 p0, p1, p2, n0, n1, n2;
            float2 uv0, uv1, uv2;
            bool ok = LoadHitTriangleAttributes(instId, primIdx,
                                                p0, p1, p2, n0, n1, n2,
                                                uv0, uv1, uv2);
            if (ok)
            {
                float u = bary.x;
                float v = bary.y;
                float w = 1.0 - u - v;
                float3 nObj = normalize(n0 * w + n1 * u + n2 * v);
                if (any(isnan(nObj)) || dot(nObj, nObj) < 1e-4)
                    nObj = normalize(cross(p1 - p0, p2 - p0));
                worldN = normalize(mul((float3x3)obj2w, nObj));
                hitUV  = uv0 * w + uv1 * u + uv2 * v;
                hasUV  = true;
            }
            else
            {
                worldN = -normalize(rayDir);
            }
        }

        // Emissive texture modulation (matches GBuffer.ps convention:
        // emissiveOut = (emissiveColor.rgb * w) * emissiveTex.rgb). The
        // constant component was pre-multiplied on the CPU side; here we
        // multiply by the texture sample at the interpolated UV.
        if (inst.emissiveTexIdx >= 0 && hasUV)
        {
            const uint texIdx = NonUniformResourceIndex((uint)inst.emissiveTexIdx);
            float3 emTex = g_AllTextures[texIdx]
                .SampleLevel(g_LinearSampler, hitUV, 0).rgb;
            emissive *= emTex;
        }

        // Direct lighting — pick one random light from the cluster GPULight
        // buffer and importance-sample by lightCount. Matches WickedEngine's
        // ddgi_raytraceCS approach: per-ray Monte-Carlo over the light list,
        // shadow-rayed, multiplied by N to undo the 1/N pick prob.
        float3 directLit = float3(0, 0, 0);
        if (g_Vol.lightCount > 0u)
        {
            uint seed = HashPCG((probeIdx * 0x9E3779B9u) ^
                                (rayIdx   * 0x85EBCA77u) ^
                                (g_Vol.frameIndex * 0xC2B2AE3Du));
            uint pick = seed % g_Vol.lightCount;
            GPULight L  = g_Lights[pick];
            directLit   = EvalLight(L, hitPos, worldN) * float(g_Vol.lightCount);
        }
        // `directLit` and `prev.rgb` are both irradiance terms (energy/area
        // arriving at the surface). For a Lambertian surface the outgoing
        // radiance is (albedo / π) * total_irradiance — one π divide.
        //
        // Small constant ambient pedestal restores the bootstrap signal that
        // indoor probes need before multi-bounce converges. Without it, rays
        // hitting deeply-shadowed surfaces return literal zero radiance, the
        // EMA mean writes 0 into SH, and probes stay invisible until enough
        // rays escape to sky to seed bouncing — which can take many frames
        // in enclosed scenes. 0.05 is a conservative floor: 0.05/π ≈ 0.016
        // of base contribution, swamped by any real direct/bounce light but
        // enough to keep dark probes from collapsing to pure black.
        float3 ambient = float3(0.05, 0.05, 0.05);
        float3 incomingIrradiance = directLit + ambient;

        // Multi-bounce gate. TWO conditions must hold:
        //   (1) frameIndex > 0  — there's a previous frame's SH to read.
        //   (2) overwrite flag (bit 3) clear — the relight has done at least
        //       one full overwrite-write of the SH buffer, so what we read
        //       here is real radiance and not the GPU's uninitialised memory.
        //
        // Without (2), the very first trace dispatch reads whatever bit
        // pattern UAV-allocated probe-SH memory happens to hold. The relight
        // would blend that into SH; the next frame's trace re-reads the now-
        // poisoned SH and the geometric series locks the probes onto the
        // initial garbage. Pairs with DDGICommon.hlsli's DDGI_SH_Irradiance
        // negative-lobe protection as defence in depth.
        if (g_Vol.frameIndex > 0 && (g_Vol.flags & 8u) == 0u)
        {
            float3 viewDir = -rayDir;
            float4 prev = DDGI_SampleVolume(hitPos, worldN, viewDir, g_Vol,
                                            g_PrevProbeSH, g_PrevDepth,
                                            g_PrevProbeData, g_LinearSampler);
            // Multi-bounce: 0.5 conservation factor (was 0.7). At 0.7 the
            // geometric series amplifies frame-to-frame trace noise by 1.4×
            // (1/sqrt(1-0.49)), which combined with the engine's 64–256 rays
            // per probe hits visible flicker even after the EMA. 0.5 drops
            // amplification to 1.15× — measurable visual contribution loss
            // (~33%) but flicker collapses to ~17% noise even at the default
            // hysteresis 0.92 instead of needing 0.99.
            //
            // The min cap also tightens (was 5.0) — most legit prev_irradiance
            // values inside the volume are <1.5 in practice, so 3.0 only
            // touches actual fireflies without dimming bright lit surfaces.
            float3 prevSafe = min(prev.rgb * prev.a, float3(3.0, 3.0, 3.0));
            incomingIrradiance += prevSafe * 0.5;
        }

        // Outgoing radiance = self-emission + Lambertian reflection. Emissive
        // is added DIRECTLY (independent of incoming light) so glowing
        // surfaces — lamps, neon signs, screens, etc. — contribute to the
        // probe SH and bleed colour onto nearby walls. Mirrors GBuffer.ps's
        // "emissive bypasses the BRDF" convention for direct illumination,
        // so direct + indirect render the same emissive intensity.
        //
        // Note on perceived strength: DDGI's probe grid samples lighting at
        // discrete points, so emissive surfaces with small solid angle (a
        // single lamp shade, a neon sign) only register on probes that are
        // close enough for the surface to occupy a meaningful fraction of
        // the unit sphere. Densify the volume's probe grid to extend the
        // emissive bleed range; a typical lamp at radius 0.5 units reaches
        // probes within ~1-2 units before its solid-angle contribution
        // drops below the 64-rays-per-probe noise floor.
        radiance    = emissive + albedo * incomingIrradiance * (1.0 / 3.14159265);
        // Tighter firefly cap (was 10) — primary-color saturation in the
        // probes was tracking back to a few rays returning radiance > 5
        // each frame, which the EMA never fully discarded. The cap also
        // bounds emissive contribution: a (10, 0, 0) red sign clamps to
        // 5.0 R, which is plenty bright for indirect-diffuse purposes.
        radiance    = min(radiance, float3(5.0, 5.0, 5.0));
        hitDistance = t;
    }

    // Hard NaN/Inf scrub before writing to the ray-data buffer. `min(x, 5.0)`
    // above does NOT strip NaN (IEEE 754: min(NaN, x) = NaN), so any source
    // of NaN in this shader (degenerate triangle attributes feeding worldN,
    // multi-bounce feedback amplifying a previous-frame probe corruption,
    // chebyshev division-by-near-zero in DDGI_SampleVolume) would otherwise
    // enter the persistent SH state and never leave. Replacing NaN with zero
    // at the write boundary breaks the feedback loop deterministically — at
    // worst a single ray contributes nothing this frame.
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = float3(0, 0, 0);

    // Storage layout uses the VOLUME's raysPerProbe as stride (buffer size
    // mirrors that). Adaptive ray count only affects how many indices [0..k)
    // are dispatched this frame; the stride stays at the volume's max.
    g_RayData[probeIdx * g_Vol.raysPerProbe + rayIdx] = float4(radiance, hitDistance);
}
