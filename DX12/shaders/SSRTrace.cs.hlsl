// SSRTrace.cs.hlsl — stochastic GGX ray gen + FidelityFX-SSSR Hi-Z traversal.
//
// Pass 2 of the Hi-Z SSR pipeline. Rewritten against Wicked Engine's
// ssr_raytraceCS.hlsl (itself ported from FidelityFX-SSSR). Known-good
// traversal math replaces my earlier hand-rolled version which hit a
// self-intersection pathology on the starting tile at mip >= 3 and produced
// degenerate L vectors → NaN weights → black output.
//
// Reverse-Z convention (engine-wide): z_ndc = 1 near, 0 far.
// 2-channel depth pyramid stores .r = max(z) = NEAREST surface in tile.
//
// Outputs (3 textures, all at render resolution):
//   u0  hitBuffer     RGBA16F  .rgb = sampled scene color at hit UV,
//                              .a   = confidence ∈ [0,1]  (miss = 0)
//   u1  rayDirPDF     RGBA16F  .rgb = world-space reflection direction L
//                              .a   = PDF of the sampled H (for BRDF reweight)
//   u2  rayLength     R16F     world-space ray distance from P to hit (0 on miss)
//
// Sampling scene color in trace (rather than deferring to resolve) matches
// Wicked's arrangement: resolve does spatial BRDF reweighting on already-
// sampled neighbour hits, so each pixel pays one pyramid fetch instead of
// nine.

cbuffer SSRCB : register(b0, space2)
{
    float4x4 viewProj;            // jittered — must match depth rasterization
    float4x4 invViewProj;         // jittered inverse
    float3   cameraPos;           float  nearZ;
    // Phase 3: trace runs at HALF-RES. renderW/H is the full-res GBuffer dim
    // (used for all GBuffer / pyramid math); traceW/H is the dispatch dim
    // (used to bound DTid + index UAV writes).
    uint     renderW;             uint   renderH;
    uint     hizMipCount;         float  maxRayLength;
    float    farZ;                float  roughnessCutoff;
    uint     frameIndex;          uint   _pad1;
    // UE-aligned tunables (all runtime-adjustable from SSRPass setters).
    //   traceThickness     — linear-Z tolerance for hit validation.
    //                        UE defaults 0.02–0.05 w.u.; 1.5 was Wicked's
    //                        default and over-accepts thin-surface tunneling.
    //   hizMostDetailedLvl — finest mip the walker visits. 0 = bruteforce
    //                        pixel-step (expensive on long rays). UE starts
    //                        from 1 or 2 and refines down.
    //   coneMipMax         — upper bound on PickConeMip. Higher = blurrier
    //                        radiance fetch for rough surfaces.
    //   depthBiasFactor    — fraction of the pixel's linear depth to push the
    //                        ray origin toward the camera by. Fixes the
    //                        classic Hi-Z self-intersection: without a bias,
    //                        origin sits exactly on the surface, the first
    //                        cell's max depth equals (or exceeds) origin.z,
    //                        `aboveSurface` never flips true, the walker
    //                        descends mips and returns "hit at origin" —
    //                        which samples the reflecting plane's own color
    //                        and makes anything touching the plane invisible
    //                        in the reflection. UE-ish values: 0.002–0.01.
    //   finishLinearSteps  — Phase 2 (Lumen-style): after the Hi-Z walker
    //                        converges to mip 0 it stops at a CELL corner,
    //                        not the actual hit pixel. Without a finish trace
    //                        the converged xy lands on whichever neighbour
    //                        pixel the cell happens to cover — often sky for
    //                        mesh-vs-sky silhouettes, causing the historical
    //                        "reflection shows sky next to objects" bug.
    //                        This many pixel-steps of linear refinement walk
    //                        the ray back along its direction one pixel at a
    //                        time until ray.z crosses surface.z, landing on
    //                        the real hit pixel. 16 is generous; UE uses ~8.
    float    traceThickness;      uint   hizMostDetailedLvl;
    float    coneMipMax;          float  depthBiasFactor;
    uint     finishLinearSteps;   uint   traceW;
    uint     traceH;              uint   _pad2;
};

// Phase 7: sub-pixel jitter table removed. Trace runs at full render res; the
// dispatch grid IS the GBuffer pixel grid, no jittering needed. The traceW/H
// CB fields stay equal to renderW/H so future re-introduction of half-res
// doesn't require another shader edit.

Texture2D<float4>   gNormal    : register(t0, space2);
Texture2D<float4>   gSurface   : register(t1, space2);
Texture2D<float>    gDepth     : register(t2, space2);
Texture2D<float2>   gDepthHier : register(t3, space2);   // 2-ch pyramid; .r=max=nearest
Texture2D<float4>   gHdrPyramid: register(t4, space2);   // pre-filtered HDR mip chain
Texture2D<float2>   gVelocity  : register(t7, space2);   // NDC delta

RWTexture2D<float4> OutHit        : register(u0, space2);
RWTexture2D<float4> OutRayDirPDF  : register(u1, space2);
RWTexture2D<float>  OutRayLength  : register(u2, space2);

SamplerState gLinClamp : register(s0, space2);

// ----------------------------------------------------------------------------
static const float PI = 3.14159265358979;
static const float blendScreenEdgeFade     = 5.0;   // vignette sharpness
static const uint  HiZTraceIterationsMax   = 256;
// traceThickness, hizMostDetailedLvl, coneMipMax now come from CB.

// ----------------------------------------------------------------------------
// Math helpers
// ----------------------------------------------------------------------------
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 clip = float4(ndc, depth, 1.0);
    float4 wp = mul(clip, invViewProj);
    return wp.xyz / wp.w;
}

float LinearizeReverseZ(float zNdc)
{
    return (nearZ * farZ) / (nearZ + (farZ - nearZ) * zNdc);
}

// Inverse of LinearizeReverseZ — eye-space Z → reverse-Z NDC.
// linZ → nearZ-far-range. Used to convert a linear-space depth offset back
// into an NDC-space start position for Hi-Z raymarch.
float InverseLinearDepth(float linZ)
{
    return (nearZ * farZ / max(linZ, 1e-4) - nearZ) / max(farZ - nearZ, 1e-4);
}

uint WangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u;
    s ^= s >> 4u;
    s *= 0x27d4eb2du;
    s ^= s >> 15u;
    return s;
}

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley2D(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

// Hammersley with per-pixel Cranley-Patterson rotation. Decorrelates
// neighbouring pixels without giving up the base sequence's low discrepancy.
float2 SampleHammersley(uint2 pixel, uint frame, uint regen)
{
    uint  sampleIdx = (frame * 15u + regen) & 63u;
    float2 Xi = Hammersley2D(sampleIdx, 64u);

    uint h = WangHash((pixel.x + pixel.y * renderW) ^ (regen * 0x9E3779B9u));
    float2 offset = float2(h & 0xFFFFu, (h >> 16) & 0xFFFFu) * (1.0 / 65536.0);
    return frac(Xi + offset);
}

// Standard GGX importance sample → tangent-space H (normal = +Z).
float4 ImportanceSampleGGX(float2 Xi, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    // VNDF PDF for reflection lobe.
    float D = (a * a) /
              (PI * pow((cosTheta * cosTheta) * (a * a - 1.0) + 1.0, 2.0));
    float pdf = D * cosTheta;
    return float4(H, pdf);
}

float3x3 GetTangentBasis(float3 N)
{
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);
    return float3x3(T, B, N);
}

// ----------------------------------------------------------------------------
// FidelityFX-SSSR Hi-Z traversal (ported from Wicked's ssr_raytraceCS.hlsl)
// ----------------------------------------------------------------------------
float2 GetMipResolution(float2 screenDim, int mipLevel)
{
    return screenDim * pow(0.5, mipLevel);
}

void InitialAdvanceRay(
    float3 origin, float3 direction,
    float2 currentMipResolution, float2 currentMipResolution_rcp,
    float2 floorOffset, float2 uvOffset,
    out float3 position, out float tCurrent)
{
    float2 currentMipPosition = currentMipResolution * origin.xy;

    // Intersect ray with the half box pointing away from origin.
    float2 xyPlane = floor(currentMipPosition) + floorOffset;
    xyPlane = xyPlane * currentMipResolution_rcp + uvOffset;

    float2 t = (xyPlane - origin.xy) / direction.xy;
    tCurrent = min(t.x, t.y);
    position = origin + tCurrent * direction;
}

bool AdvanceRay(
    float3 origin, float3 direction,
    float2 currentMipPosition, float2 currentMipResolution_rcp,
    float2 floorOffset, float2 uvOffset,
    float surfaceZ,
    inout float3 position, inout float tCurrent)
{
    float2 xyPlane = floor(currentMipPosition) + floorOffset;
    xyPlane = xyPlane * currentMipResolution_rcp + uvOffset;
    float3 boundaryPlanes = float3(xyPlane, surfaceZ);

    float3 t = (boundaryPlanes - origin) / direction;

    // Only use the z-plane when marching toward the far plane.
    // Reverse-Z: rayDir.z < 0 means going from near to far (ray can hit surfaces ahead).
    t.z = direction.z < 0 ? t.z : 1e30;

    float tMin = min(min(t.x, t.y), t.z);

    // Reverse-Z: "larger z means closer to camera", so position is ABOVE the
    // surface when position.z > surfaceZ.
    bool aboveSurface = surfaceZ < position.z;

    // asuint bit-equality to detect "did we clamp at z-plane exactly?" — NaN-safe.
    bool skippedTile = asuint(tMin) != asuint(t.z) && aboveSurface;

    tCurrent = aboveSurface ? tMin : tCurrent;
    position = origin + tCurrent * direction;

    return skippedTile;
}

// Hierarchical ray march in screen space [0,1]² × [0,1].
float3 HierarchicalRaymarch(
    float3 origin, float3 direction, float2 screenDim,
    out bool validHit)
{
    int mostDetailed = (int)hizMostDetailedLvl;
    int currentMip   = mostDetailed;

    float2 currentMipResolution     = GetMipResolution(screenDim, currentMip);
    float2 currentMipResolution_rcp = rcp(currentMipResolution);

    // Slight overshoot so the ray consistently crosses boundaries without
    // getting trapped on shared edges (Wicked's uvOffset trick).
    float2 uvOffset = 0.005 * exp2((float)mostDetailed) / screenDim;
    uvOffset = direction.xy < 0 ? -uvOffset : uvOffset;

    // Which corner of the cell the ray exits through.
    float2 floorOffset = direction.xy < 0 ? 0 : 1;

    float  tCurrent;
    float3 position;
    InitialAdvanceRay(origin, direction, currentMipResolution,
                      currentMipResolution_rcp, floorOffset, uvOffset,
                      position, tCurrent);

    int i = 0;
    int maxMip = (int)hizMipCount - 1;
    [loop]
    while (i < (int)HiZTraceIterationsMax && currentMip >= mostDetailed)
    {
        if (any(position.xy < 0.0) || any(position.xy > 1.0))
        {
            validHit = false;
            return position;
        }

        float2 currentMipPosition = currentMipResolution * position.xy;
        // Reverse-Z: .r = max depth = nearest surface in tile.
        float  surfaceZ = gDepthHier.Load(int3(currentMipPosition, currentMip)).r;

        bool skippedTile = AdvanceRay(origin, direction,
                                      currentMipPosition, currentMipResolution_rcp,
                                      floorOffset, uvOffset,
                                      surfaceZ, position, tCurrent);

        currentMip         += skippedTile ? 1 : -1;
        currentMip          = min(currentMip, maxMip);
        currentMipResolution *= skippedTile ? 0.5 : 2.0;
        currentMipResolution_rcp *= skippedTile ? 2.0 : 0.5;

        i++;
    }

    validHit = (i < (int)HiZTraceIterationsMax);
    return position;
}

// ----------------------------------------------------------------------------
// Perspective-correct NDC z of the world ray at a given screen UV.
//
// The world ray P(s) = worldPos + s·L maps to clip(s) = startClip + s·dirClip
// (linear in s). The screen UV at parameter s is:
//   ndc(s).xy = clip(s).xy / clip(s).w
//   uv(s)     = ndc(s).xy * (0.5, -0.5) + 0.5
// Given a target uv, back-solve for s along whichever axis has the bigger
// denominator (numerically stable), then evaluate ndc(s).z = clip(s).z /
// clip(s).w. The linear `cur.z = startSS.z + t·rayDirSS.z` the Hi-Z + finish
// walker uses underestimates this real z for reverse-Z perspective (NDC z is
// convex in t under reverse-Z), so a linear cur.z crosses surface depth
// EARLIER than the real ray would — the walker stops short and reports a
// hit pixel closer to the start than truth. The further from the camera the
// reflecting surface is, the larger this error grows, which is the visible
// "reflection slides as camera pulls away" symptom.
float EvalRayNdcZ(float2 uv, float4 sClip, float4 dClip)
{
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = 1.0 - uv.y * 2.0;
    float denX = ndcX * dClip.w - dClip.x;
    float denY = ndcY * dClip.w - dClip.y;
    float s = (abs(denX) > abs(denY))
              ? (sClip.x - ndcX * sClip.w) / denX
              : (sClip.y - ndcY * sClip.w) / denY;

    // The world ray P(s)=worldPos+s·L projects to a straight line on screen,
    // but only the s>=0, clipW>0 portion corresponds to points IN FRONT of
    // the camera. Past the vanishing point (asymptote on screen — happens
    // when L has a strong forward component), back-solving uv to s gives
    // s<0 / negative clipW which describes a phantom point BEHIND the
    // camera. Plugging that into clipZ/clipW yields a negative or
    // nonsensical depth that the walker would always accept as
    // realZ<=sZ — producing a fake hit at the first past-asymptote pixel
    // and clustering every floor reflection onto a single screen region.
    // Sentinel >>1 so the walker treats these pixels as "ray doesn't
    // reach here", continues, and eventually exits off-screen → falls back
    // to the IBL probe via the trace's miss path.
    float clipW = sClip.w + s * dClip.w;
    if (s < 0.0 || clipW < 1e-3) return 1e9;

    float clipZ = sClip.z + s * dClip.z;
    return clipZ / clipW;
}

// ----------------------------------------------------------------------------
// Hi-Z + perspective-correct linear finish trace.
//
// Hi-Z (HierarchicalRaymarch) skips empty tiles fast and converges near the
// true crossing pixel. The depth pyramid it samples MUST be correct (mips
// 1..N-1 have real max-depth values) — the per-mip CB race condition in
// SSRDepthHierarchyPass was causing every reduce dispatch to read the LAST
// iteration's dims and early-out, leaving every middle mip at 0. That made
// `aboveSurface = 0 < cur.z` always true, so Hi-Z walked off-screen for many
// rays. With that fixed, Hi-Z lands within ~1 cell of the true intersection
// and this finish trace just refines via pixel-precise backward + forward
// walks using perspective-correct realZ.
bool LinearFinishTrace(float3 hzPosition, float3 origin, float3 direction,
                       float4 sClip, float4 dClip,
                       float2 screenDim, uint maxSteps,
                       out float3 outHit, out float outSurfZ)
{
    float2 dirPxLen = abs(direction.xy) * screenDim;
    float  pxLen    = max(max(dirPxLen.x, dirPxLen.y), 1e-6);
    float  dt       = 1.0 / pxLen;       // 1 t-unit = 1 dominant-axis pixel
    float3 step     = direction * dt;

    float3 cur = hzPosition;
    int2   ssize = int2(screenDim);

    // 1. Back up while we're below the surface (real z ≤ surface z) or
    //    hitting sky. Goal: cur lands strictly ABOVE a real surface so the
    //    forward walk has a clean above→below crossing to find.
    [loop]
    for (uint b = 0; b < maxSteps / 2u; ++b)
    {
        int2  px    = clamp(int2(cur.xy * screenDim), int2(0, 0), ssize - 1);
        float sZ    = gDepth.Load(int3(px, 0));
        float realZ = EvalRayNdcZ(cur.xy, sClip, dClip);
        if (sZ > 0.0 && realZ > sZ) { cur.z = realZ; break; }
        cur -= step;
        if (any(cur.xy < 0.0) || any(cur.xy > 1.0)) { cur += step; break; }
    }

    // 2. Walk forward 1 pixel at a time until the real (perspective-correct)
    //    ray z crosses the surface. On crossing, LINEARLY INTERPOLATE between
    //    the previous (above) and current (below) sample to recover the
    //    sub-pixel hit UV — without this, adjacent reflective pixels whose
    //    rays differ by < 1 pixel of step.xy snap to the same OR adjacent
    //    integer hit pixels, producing visible staircase / pixel-jump
    //    artifacts on slanted reflected surfaces (worst at high-contrast
    //    edges like column silhouettes on the right side of the floor).
    float3 prev    = cur;
    float  prevRZ  = EvalRayNdcZ(cur.xy, sClip, dClip);
    [loop]
    for (uint f = 0; f < maxSteps; ++f)
    {
        if (any(cur.xy < 0.0) || any(cur.xy > 1.0)) break;
        int2  px    = clamp(int2(cur.xy * screenDim), int2(0, 0), ssize - 1);
        float sZ    = gDepth.Load(int3(px, 0));
        float realZ = EvalRayNdcZ(cur.xy, sClip, dClip);
        if (sZ > 0.0 && realZ <= sZ)
        {
            // Sub-pixel crossing: solve realZ(t) = sZ between prev and cur
            // assuming sZ ≈ constant across the 1-pixel span and realZ is
            // ≈ linear between adjacent walker steps.
            //
            // Range-check t (do NOT saturate). When the surface depth jumps
            // discontinuously between prev and cur (typical at the silhouette
            // of a foreground object floating above the receiving surface:
            // prev's sZ = far-background, cur's sZ = near-foreground sphere),
            // both prev.realZ and cur.realZ stay nearly equal across the one
            // pixel step, so the algebraic "crossing point" with the assumed
            // constant sZ lies OUTSIDE [prev, cur]. Saturating to [0,1] would
            // pin the lerp to one endpoint and report a "hit" that's actually
            // a fake intersection — the world ray missed the foreground in 3D
            // and the screen-space "crossing" is purely the surface-depth
            // discontinuity. Skip those and keep marching; the real ray
            // either crosses the foreground further along OR exits screen.
            float denom = prevRZ - realZ;
            float t_raw = (abs(denom) > 1e-6) ? (prevRZ - sZ) / denom : 1.0;
            if (t_raw >= -0.01 && t_raw <= 1.01)
            {
                float t   = saturate(t_raw);
                outSurfZ  = sZ;
                outHit.xy = lerp(prev.xy, cur.xy, t);
                outHit.z  = lerp(prevRZ,  realZ,  t);
                return true;
            }
            // Discontinuity false crossing: fall through to keep marching.
        }
        prev   = cur;
        prevRZ = realZ;
        cur   += step;
    }

    // Budget exhausted — fall back to the Hi-Z position with its own surface
    // depth so the caller's ValidateHit can decide via thickness check.
    int2 px = clamp(int2(hzPosition.xy * screenDim), int2(0, 0), ssize - 1);
    outSurfZ = gDepth.Load(int3(px, 0));
    outHit   = hzPosition;
    return false;
}

// ----------------------------------------------------------------------------
// Hit validation (Wicked's pattern: edge vignette × depth-thickness)
// ----------------------------------------------------------------------------
float CalculateEdgeVignette(float2 hitUV)
{
    float2 hitNDC = hitUV * 2.0 - 1.0;
    float2 vignette = saturate(abs(hitNDC) * blendScreenEdgeFade -
                               (blendScreenEdgeFade - 1.0));
    return saturate(1.0 - dot(vignette, vignette));
}

// ray-end depth (hit.z) vs the surface depth at the same UV — reject if the
// ray goes THROUGH a thin surface by too much.
float ValidateHit(float3 hit, float surfaceDepthAtHit, float2 prevHitUV)
{
    float vignette     = CalculateEdgeVignette(hit.xy);
    float vignettePrev = CalculateEdgeVignette(prevHitUV);
    vignette = min(vignette, vignettePrev);

    // Depth delta → linear thickness. `traceThickness` is CB-supplied so
    // artists / renderer can dial per scene; UE ships 0.02–0.05 world units.
    float linRay = LinearizeReverseZ(hit.z);
    float linSrf = LinearizeReverseZ(surfaceDepthAtHit);
    float thickness = abs(linRay - linSrf);
    float confidence = 1.0 - smoothstep(0.0, max(traceThickness, 1e-4), thickness);

    return vignette * confidence;
}

// ----------------------------------------------------------------------------
// Cone-footprint mip for the scene color pyramid (roughness × ray length).
// Upper bound comes from CB (coneMipMax). Higher caps give blurrier rough
// reflections but a too-aggressive blur on a low-res mip bleeds whatever is
// *around* small reflected objects (typically sky) into the sample. UE
// stochastic SSR relies on spatial + temporal to do most of the low-pass
// work rather than yanking the cone up; 3–4 is a reasonable default here.
// ----------------------------------------------------------------------------
float PickConeMip(float roughness, float rayLen, float hitLinZ)
{
    float coneRadius  = roughness * roughness * rayLen;
    float pixelRadius = coneRadius * float(renderH) * 0.5 / max(hitLinZ, 0.1);
    return clamp(log2(max(pixelRadius, 1.0)), 0.0, coneMipMax);
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ----------------------------------------------------------------------------
// Trace ONE reflection ray for `pixel` along world-space direction L.
// Returns true + (hitColor, confidence, rayLen) on a validated hit; false on
// any miss/reject. Factored out of CSMain so the multi-sample loop can average
// several independent rays per pixel — the early-outs that used to `return`
// from the whole pixel become `return false` so one bad sample just drops out.
// Body is the original single-ray path verbatim (projection → Hi-Z walk →
// linear finish → validate → cone-mip fetch).
// ----------------------------------------------------------------------------
bool TraceOneReflection(uint2 pixel, float3 worldPos, float depth,
                        float roughness, float3 L,
                        out float3 hitColor, out float confidence, out float rayLen)
{
    hitColor   = float3(0, 0, 0);
    confidence = 0.0;
    rayLen     = 0.0;

    float4 startClip = mul(float4(worldPos,     1.0), viewProj);
    float4 endClip   = mul(float4(worldPos + L, 1.0), viewProj);
    if (startClip.w <= 1e-3) return false;        // origin behind near plane: bail

    float3 startSS;
    {
        float3 startNDC = startClip.xyz / startClip.w;
        startSS.xy      = startNDC.xy * float2(0.5, -0.5) + 0.5;
        startSS.z       = startNDC.z;

        float linZ       = LinearizeReverseZ(startSS.z);
        float biasAbs    = max(linZ * depthBiasFactor, 0.02);
        float linZBiased = max(linZ - biasAbs, nearZ);
        startSS.z        = saturate(InverseLinearDepth(linZBiased));
    }

    const float kClipW = 1e-3;
    if (endClip.w <= kClipW)
    {
        float denom = endClip.w - startClip.w;
        float t     = saturate((kClipW - startClip.w) / denom);
        endClip     = lerp(startClip, endClip, t);
    }

    float3 endSS = endClip.xyz / endClip.w;
    endSS.xy = endSS.xy * float2(0.5, -0.5) + 0.5;

    float3 rayDirSS = endSS - startSS;

    {
        float2 invDir = rcp(abs(rayDirSS.xy) + 1e-6);
        float2 tEdge  = (rayDirSS.xy > 0 ? (1.0 - startSS.xy) : startSS.xy) * invDir;
        float  tMax   = max(min(tEdge.x, tEdge.y), 1.0);
        rayDirSS     *= tMax;
    }

    bool  validHit = false;
    float3 hit = HierarchicalRaymarch(startSS, rayDirSS,
                                      float2(renderW, renderH), validHit);
    if (!validHit) return false;

    float4 dirClip = mul(float4(L, 0.0), viewProj);
    float  surfaceDepthAtHit;
    bool   finishOk = LinearFinishTrace(hit, startSS, rayDirSS,
                                        startClip, dirClip,
                                        float2(renderW, renderH),
                                        max(finishLinearSteps, 1u),
                                        hit, surfaceDepthAtHit);

    if (surfaceDepthAtHit <= 0.0) return false;   // refined hit landed on sky

    confidence = ValidateHit(hit, surfaceDepthAtHit, startSS.xy);

    {
        int2 hitPxFull = clamp(int2(hit.xy * float2(renderW, renderH)),
                               int2(0, 0), int2(renderW - 1, renderH - 1));
        float3 hitN  = normalize(gNormal.Load(int3(hitPxFull, 0)).rgb * 2.0 - 1.0);
        float  front = dot(hitN, -L);
        confidence  *= smoothstep(0.05, 0.20, front);
    }
    if (confidence <= 0.0) return false;

    float3 hitWS     = ReconstructWorldPos(hit.xy, surfaceDepthAtHit);
    rayLen           = length(hitWS - worldPos);
    float  hitLinZ   = LinearizeReverseZ(surfaceDepthAtHit);
    float  startLinZ = LinearizeReverseZ(depth);
    float  minRayLen = max(startLinZ * 0.005, 0.01);
    if (rayLen < minRayLen) return false;         // self-intersection

    float  mipLvl   = (roughness < 0.1)
                      ? 1.0
                      : PickConeMip(roughness, rayLen, hitLinZ);
    hitColor = gHdrPyramid.SampleLevel(gLinClamp, hit.xy, mipLvl).rgb;
    return true;
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    // Phase 7: full-res dispatch — one thread per GBuffer pixel.
    if (DTid.x >= traceW || DTid.y >= traceH) return;

    const uint2  pixel = DTid.xy;
    const float2 uv    = (float2(pixel) + 0.5) / float2(renderW, renderH);

    // Write miss by default; early-returns below leave these values.
    OutHit[pixel]       = 0;
    OutRayDirPDF[pixel] = 0;
    OutRayLength[pixel] = 0;

    float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0) return;                     // sky

    float4 surf = gSurface.Load(int3(pixel, 0));
    float  roughness = max(surf.r, 0.045);
    if (roughness > roughnessCutoff) return;      // rougher → probe fallback

    float3 N = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 V = normalize(cameraPos - worldPos);

    // --- Multi-sample reflection -------------------------------------------
    // Average kTraceSamples independent reflection rays per pixel per frame.
    // A single stochastic GGX ray is too noisy for the spatial + temporal
    // denoiser on CURVED / low-coverage glossy surfaces (a basin, a sphere):
    // neighbours diverge so the resolve has few valid taps, and the reflected
    // content reprojects poorly so the temporal pass keeps rejecting history —
    // which is why those surfaces previously needed a heavy TAA history crutch
    // (ghosting) to look stable. Averaging N rays cuts the per-frame variance
    // ~1/sqrt(N) at the SOURCE, independent of neighbours or history, so SSR
    // stops leaning on TAA. Raise kTraceSamples for cleaner curved reflections
    // (cost is N× the Hi-Z walk, the trace's heavy part); 1 = old behaviour.
    // Mirror-deterministic surfaces (roughness <= kMirrorThreshold) carry no
    // stochastic noise, so they always take a single sample.
    const float kMirrorThreshold = 0.05;
    const uint  kMaxRegen        = 15u;
    const uint  kTraceSamples    = 4u;

    const bool isMirror = (roughness <= kMirrorThreshold);
    const uint nSamples = isMirror ? 1u : kTraceSamples;
    float3x3   TBN      = GetTangentBasis(N);   // glossy GGX basis (hoisted)

    float3 accumColor = float3(0, 0, 0);   // Σ Karis(color) * confidence
    float  accumW     = 0.0;               // Σ confidence over VALID hits
    float  sumConf    = 0.0;               // Σ confidence over ALL attempts (miss = 0)
    float  bestRayLen = 1e9;
    float3 bestL      = reflect(-V, N);
    float  bestPdf    = 1.0;
    uint   nValid     = 0u;

    [loop]
    for (uint s = 0; s < nSamples; ++s)
    {
        // ---- Pick this sample's reflection direction ----
        float3 L; float pdf; float RdotN = 0.0;
        if (isMirror)
        {
            L = reflect(-V, N); pdf = 1.0; RdotN = dot(N, L);
            if (RdotN <= 0.01) break;
        }
        else
        {
            [loop]
            for (uint r = 0; r < kMaxRegen && RdotN <= 0.01; ++r)
            {
                // Fold the sample index s into the regen counter so each of the
                // N samples draws a DISTINCT Halton point (SampleHammersley also
                // decorrelates per pixel + per frame).
                float2 Xi   = SampleHammersley(pixel, frameIndex, s * kMaxRegen + r);
                float4 Hpdf = ImportanceSampleGGX(Xi, roughness);
                float3 H    = mul(Hpdf.xyz, TBN);
                L           = reflect(-V, H);
                pdf         = Hpdf.w;
                RdotN       = dot(N, L);
            }
            if (RdotN <= 0.01) continue;       // no valid ray this sample
        }

        // ---- Trace it ----
        float3 hc; float conf; float rl;
        if (!TraceOneReflection(pixel, worldPos, depth, roughness, L, hc, conf, rl))
            continue;

        // Karis-weighted, confidence-weighted accumulate (firefly-robust mean,
        // matching the resolve so one sky hit can't swamp the average).
        float Yin = max(Luminance(hc), 0.0);
        accumColor += hc * rcp(1.0 + Yin) * conf;
        accumW     += conf;
        sumConf    += conf;
        nValid++;
        if (rl < bestRayLen) { bestRayLen = rl; bestL = L; bestPdf = pdf; }
    }

    if (nValid == 0u || accumW <= 1e-6) return;   // all rays missed → default miss stays

    // Karis inverse-expand the weighted mean back to HDR.
    float3 meanColor = accumColor / accumW;
    float  Yavg      = saturate(Luminance(meanColor));
    meanColor       *= rcp(max(1.0 - Yavg, 1e-3));

    // Confidence averaged over EVERY attempted sample (misses score 0) so a
    // pixel whose lobe partly leaves the screen fades rather than pops.
    float outConf = sumConf / float(nSamples);

    OutHit[pixel]       = float4(meanColor, outConf);
    // Representative ray = nearest valid hit, for the resolve's parallax
    // reconstruction + the temporal reflection-reprojection.
    OutRayDirPDF[pixel] = float4(bestL, bestPdf);
    OutRayLength[pixel] = bestRayLen;
}
