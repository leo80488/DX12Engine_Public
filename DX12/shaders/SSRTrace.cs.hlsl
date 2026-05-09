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
    uint     screenW;             uint   screenH;
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
    float    traceThickness;      uint   hizMostDetailedLvl;
    float    coneMipMax;          float  depthBiasFactor;
};

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

    uint h = WangHash((pixel.x + pixel.y * screenW) ^ (regen * 0x9E3779B9u));
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
    float pixelRadius = coneRadius * float(screenH) * 0.5 / max(hitLinZ, 0.1);
    return clamp(log2(max(pixelRadius, 1.0)), 0.0, coneMipMax);
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= screenW || DTid.y >= screenH) return;
    const uint2  pixel = DTid.xy;
    const float2 uv    = (float2(pixel) + 0.5) / float2(screenW, screenH);

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

    // --- Ray regeneration loop (Wicked-style) -------------------------------
    // Some GGX samples produce H vectors whose reflected L is below the
    // surface (RdotN <= 0). Re-draw another low-discrepancy sample — cheap
    // compared to tracing a wasted ray.
    float3 L;
    float  pdf;
    float  RdotN = 0.0;
    const uint kMaxRegen = 15;
    {
        float3x3 TBN = GetTangentBasis(N);
        [loop]
        for (uint r = 0; r < kMaxRegen && RdotN <= 0.01; ++r)
        {
            float2 Xi = SampleHammersley(pixel, frameIndex, r);
            float4 Hpdf = ImportanceSampleGGX(Xi, roughness);
            float3 H    = mul(Hpdf.xyz, TBN);     // tangent → world
            L           = reflect(-V, H);
            pdf         = Hpdf.w;
            RdotN       = dot(N, L);
        }
        if (RdotN <= 0.01) return;                // couldn't find a valid ray
    }

    // --- Project ray into screen space --------------------------------------
    // Ray origin uses the pixel's own (uv, depth) DIRECTLY — no round-trip
    // through invViewProj × viewProj. Matrix-inverse rounding offsets the
    // startSS by ~1 texel; that offset makes Hi-Z's first surfaceZ fetch
    // disagree with position.z, `aboveSurface` flips, and the ray either
    // self-stops or tunnels the floor.
    //
    // Depth bias: push the start depth toward the camera in LINEAR space by
    // a small fraction of the pixel's linear depth. Without this the ray
    // origin sits exactly on the surface — the first Hi-Z cell's max-depth
    // (nearest of the 4 neighbours at mip 1) is >= origin.z, `aboveSurface`
    // never flips true, the walker descends mips and immediately reports
    // "hit at origin". The reflection then samples this plane's own colour,
    // so objects sitting on the plane vanish in the reflection and the
    // plane's diffuse tint leaks into every hit colour.
    //
    // UE biases by a fraction of the GBuffer depth (scale-independent); 0.5%
    // is a reasonable middle ground between "still self-hits" and "reflection
    // visibly floats above caster". Tune via SSRPass::SetTraceParams.
    float3 startSS = float3(uv, depth);
    {
        // Depth bias: push the start toward the camera in linear space by
        // max(fractional, absolute) units. The absolute floor matters for
        // close-to-camera surfaces where a pure percentage bias (0.5% of
        // a few cm) is smaller than the Hi-Z tile's own max-vs-centre
        // depth spread — the walker's first aboveSurface test stays false
        // and the ray self-hits at iter 0 before moving. 2 cm absolute
        // floor is safe at typical scene scales.
        float linZ         = LinearizeReverseZ(depth);
        float biasAbs      = max(linZ * depthBiasFactor, 0.02);
        float linZBiased   = max(linZ - biasAbs, nearZ);
        startSS.z          = saturate(InverseLinearDepth(linZBiased));
    }

    // Endpoint: project (worldPos + L) with unit-length L to recover the
    // ray's natural screen-space direction. When the endpoint falls BEHIND
    // the near plane (endClip.w <= 0, typical at tilted / top-down views
    // where the reflected L points up-and-back toward the camera), clip
    // the ray IN CLIP SPACE so the endpoint lands just in front of the
    // near plane.
    //
    // The previous fallback ("force endSS.z = 0 = reverse-Z far plane")
    // produced a rayDirSS.z FAR longer than the true ray slope — Hi-Z
    // walked through every depth layer in one iteration and reported
    // phantom hits deep in sky. That was the visible "only works at
    // horizontal views" symptom: tilt the camera down to look at a
    // reflective platform, endClip.w went negative, the fallback fired,
    // and reflections broke for every pixel whose L reached behind-camera
    // space.
    //
    // Clip-space lerp is legitimate because mul(worldPos + t*L, viewProj)
    // is linear in t, so we can solve for t such that w = small epsilon
    // directly on the already-computed clip-space points.
    float4 startClip = mul(float4(worldPos,     1.0), viewProj);
    float4 endClip   = mul(float4(worldPos + L, 1.0), viewProj);

    const float kClipW = 1e-3;
    if (endClip.w <= kClipW)
    {
        // Visible-surface start point must sit in front of the near plane;
        // if it doesn't, GBuffer depth is bogus — bail.
        if (startClip.w <= kClipW) return;
        float denom = endClip.w - startClip.w;              // < 0 here
        float t     = saturate((kClipW - startClip.w) / denom);
        endClip     = lerp(startClip, endClip, t);
    }

    float3 endSS = endClip.xyz / endClip.w;
    endSS.xy = endSS.xy * float2(0.5, -0.5) + 0.5;

    // RAW screen-space direction: rayEndScreen - rayStartScreen.
    float3 rayDirSS = endSS - startSS;

    // Uniform-scale extension to screen edge. Unit-length L projected to
    // a far-from-camera pixel covers only a handful of screen pixels in
    // xy; the walker's `t = (xyPlane - origin.xy) / direction.xy` then
    // blows up and even if it advances correctly the 256-iter budget
    // runs out before reaching any meaningful hit. Scaling rayDirSS by
    // a uniform factor k is mathematically equivalent (tCurrent scales
    // inversely — same path), but puts t back in a well-conditioned
    // range and lets the walker span the full screen on its ray budget.
    //
    // Crucially: uniform scale across xyz preserves the ray's z/xy
    // slope, so Hi-Z's aboveSurface test still triggers at the right
    // depth. An earlier version renormalised xy to a fixed 2.0 which
    // left z unscaled — that broke steep rays because z lost its slope
    // relative to xy.
    {
        float2 invDir = rcp(abs(rayDirSS.xy) + 1e-6);
        float2 tEdge  = (rayDirSS.xy > 0 ? (1.0 - startSS.xy) : startSS.xy) * invDir;
        float  tMax   = max(min(tEdge.x, tEdge.y), 1.0);
        rayDirSS     *= tMax;
    }

    // --- Hi-Z traversal -----------------------------------------------------
    bool  validHit = false;
    float3 hit = HierarchicalRaymarch(startSS, rayDirSS,
                                      float2(screenW, screenH), validHit);
    if (!validHit) return;

    // --- Hi-Z silhouette recovery + minimal validation ---------------------
    // Hi-Z walks per-CELL. At a mesh-vs-sky silhouette the coarser-mip
    // cell's max-depth equals the MESH's depth, so position.z stops near
    // mesh depth. But when it descends to mip 0 the xy can land on any
    // pixel within the cell — frequently a SKY pixel adjacent to the
    // actual mesh pixel. Reading depth at the sky pixel gives 0, the
    // gap with hit.z explodes to (farZ-nearZ), and ValidateHit rejected
    // every mesh hit no matter how high traceThickness was set.
    //
    // 3×3 snap: search neighbours, pick the one whose linear depth best
    // matches the ray's stopping depth. The actual mesh pixel will be
    // there (Hi-Z's xy error is < 1 cell at mip 0). hit.xy is updated
    // so downstream colour sampling reads the correct (mesh, not sky)
    // pixel.
    int2 hitPxInt = clamp(int2(hit.xy * float2(screenW, screenH)),
                          int2(0, 0), int2((int)screenW - 1, (int)screenH - 1));
    float  surfaceDepthAtHit = gDepth.Load(int3(hitPxInt, 0));
    float  linRayDepth       = LinearizeReverseZ(hit.z);
    float  linHit0           = (surfaceDepthAtHit > 0.0)
                               ? LinearizeReverseZ(surfaceDepthAtHit)
                               : farZ;
    float  bestDepthDiff     = abs(linRayDepth - linHit0);
    int2   bestPxInt         = hitPxInt;

    [loop] for (int dy = -1; dy <= 1; ++dy)
    {
        [loop] for (int dx = -1; dx <= 1; ++dx)
        {
            int2 p = clamp(hitPxInt + int2(dx, dy),
                           int2(0, 0), int2((int)screenW - 1, (int)screenH - 1));
            float d = gDepth.Load(int3(p, 0));
            if (d > 0.0)
            {
                float linD = LinearizeReverseZ(d);
                float diff = abs(linRayDepth - linD);
                if (diff < bestDepthDiff)
                {
                    bestDepthDiff     = diff;
                    bestPxInt         = p;
                    surfaceDepthAtHit = d;
                }
            }
        }
    }
    hit.xy = (float2(bestPxInt) + 0.5) / float2(screenW, screenH);

    // No depth-thickness gate — every walker hit (whether on mesh or pure
    // sky) gets emitted with full confidence. The previous gate based on
    // bestDepthDiff > traceThickness was killing every mesh hit where the
    // 3×3 snap couldn't find a mesh neighbour (Hi-Z xy error > 1 pixel),
    // and those rays then showed up as IBL fallback (sky cubemap colour),
    // making SSR look "always the same colour" no matter how high the
    // intensity slider went. Now SSR always contributes; composite still
    // dampens via Fresnel × envBRDF so over-shoot stays bounded.
    float confidence = 1.0;

    // Self-intersection reject — Hi-Z occasionally bottoms out on the
    // origin's own cell. Keep this; without it self-hits draw the floor's
    // own colour as its reflection (visually correct nowhere).
    float3 hitWS     = ReconstructWorldPos(hit.xy, surfaceDepthAtHit);
    float  rayLen    = length(hitWS - worldPos);
    float  hitLinZ   = LinearizeReverseZ(surfaceDepthAtHit);
    float  startLinZ = LinearizeReverseZ(depth);
    float  minRayLen = max(startLinZ * 0.005, 0.01);
    if (rayLen < minRayLen) return;

    // Cone-footprint mip from the pre-filtered scene-color pyramid.
    // For mirror / near-mirror surfaces (mipLvl < 0.5) we point-load mip 0
    // at the snap-corrected pixel — bilinear at mip 0 would still bleed
    // sky across silhouette edges. For rougher reflections the pyramid's
    // higher mips have already smoothed silhouettes via Karis-firefly
    // reduce, so SampleLevel at the requested mip is safe and gives
    // pre-integrated radiance (variance reduction).
    float  mipLvl = PickConeMip(roughness, rayLen, hitLinZ);
    float3 hitColor;
    if (mipLvl < 0.5)
    {
        hitColor = gHdrPyramid.Load(int3(bestPxInt, 0)).rgb;
    }
    else
    {
        float2 sampleUV = (float2(bestPxInt) + 0.5) /
                          float2(screenW, screenH);
        hitColor = gHdrPyramid.SampleLevel(gLinClamp, sampleUV, mipLvl).rgb;
    }

    OutHit[pixel]       = float4(hitColor, confidence);
    OutRayDirPDF[pixel] = float4(L, pdf);
    OutRayLength[pixel] = rayLen;
}
