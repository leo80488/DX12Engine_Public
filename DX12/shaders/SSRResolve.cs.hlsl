// SSRResolve.cs.hlsl — spatial BRDF reweighting only.
//
// Pass 3 of the Hi-Z SSR pipeline. Temporal was promoted out of this shader
// into SSRTemporal.cs.hlsl; upsample runs after that to bilateral-blur rough
// regions using the variance output written here.
//
// Inputs:
//   t3 hitBuffer   (color.rgb + conf.a from trace)
//   t4 rayDirPDF   (world-space L + sampled PDF from trace)
//   t5 rayLength   (world-space ray distance from trace)
// Outputs:
//   u0 resolveColor   RGBA16F  reweighted reflection (rgb) + accumulated conf (a)
//   u1 variance       R16F     Welford weighted variance of sample luminance
//   u2 reprojDepth    R16F     NDC depth at the CLOSEST neighbour hit
//                              (linear + closest rayLength → inverse-linear)
//                              consumed by temporal for reflection-based reproj.

cbuffer SSRResolveCB : register(b0, space2)
{
    float4x4 invViewProj;
    float3   cameraPos;   float  _pad0;
    // Phase 7: dispatch + outputs at FULL-RES (traceW/H ≡ renderW/H). The CB
    // keeps both names so the shader stays half-res ready if we re-introduce
    // that path with a TAA-independent denoiser.
    uint     traceW;      uint   traceH;
    float    invTraceW;   float  invTraceH;
    float    nearZ;       float  farZ;
    uint     frameIndex;  uint   renderW;
    float    fireflyCap;  uint   renderH;
};

// Phase 7: jitter table removed — resolve runs at full render res; traceW/H
// equals renderW/H. Kept the renderW/H CB fields so the shader stays half-res
// ready if we re-introduce that path with a TAA-independent denoiser.

Texture2D<float4>    gNormal    : register(t0, space2);
Texture2D<float4>    gSurface   : register(t1, space2);
Texture2D<float>     gDepth     : register(t2, space2);
Texture2D<float4>    gHitBuffer : register(t3, space2);
Texture2D<float4>    gRayDirPDF : register(t4, space2);
Texture2D<float>     gRayLength : register(t5, space2);

RWTexture2D<float4>  OutColor        : register(u0, space2);
RWTexture2D<float>   OutVariance     : register(u1, space2);
RWTexture2D<float>   OutReprojDepth  : register(u2, space2);

static const float PI = 3.14159265358979;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp = mul(float4(ndc, depth, 1.0), invViewProj);
    return wp.xyz / wp.w;
}

float LinearizeReverseZ(float zNdc)
{
    return (nearZ * farZ) / (nearZ + (farZ - nearZ) * zNdc);
}

// Inverse of LinearizeReverseZ — takes a linear eye-space Z, returns NDC z
// under reverse-Z. Used to emit reprojectionDepth.
float InverseLinearDepth(float linZ)
{
    return (nearZ * farZ / max(linZ, 1e-3) - nearZ) / (farZ - nearZ);
}

float D_GGX(float NdotH, float a2)
{
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float V_SmithGGXCorrelated(float NdotV, float NdotL, float a2)
{
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(GGXV + GGXL, 1e-6);
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Parallax-corrected, lobe-aligned BRDF resolve weight (Stachowiak, SIGGRAPH
// 2015 "Stochastic Screen-Space Reflections"). Rather than reusing the
// neighbour's traced direction verbatim, reconstruct the neighbour's HIT point
// in world space and ask what direction THIS pixel would have to reflect along
// to see that same point. Evaluating the current pixel's GGX BRDF along that
// parallax-corrected L is what makes glossy reflections on CURVED / OBLIQUE
// surfaces resolve sharply instead of smearing the neighbour's mismatched
// reflection into the spatial mean.
float GetWeight(int2 np, float npDepth, float3 P, float3 V, float3 N,
                float roughness, float NdotV, float3 Rmirror, float selfRayLen)
{
    float4 lpdf = gRayDirPDF.Load(int3(np, 0));
    if (lpdf.w <= 0.0 || dot(lpdf.xyz, lpdf.xyz) < 1e-6) return 0.0;

    float3 npL  = normalize(lpdf.xyz);
    float  PDF  = lpdf.w;
    float  npRL = gRayLength.Load(int3(np, 0));

    // Neighbour surface pos → its reflected hit point in world space.
    float2 npUV   = (float2(np) + 0.5) / float2(renderW, renderH);
    float3 npPos  = ReconstructWorldPos(npUV, npDepth);
    float3 hitPos = npPos + npL * npRL;

    // Parallax-corrected L: from THIS pixel toward the shared hit point.
    float3 L     = normalize(hitPos - P);
    float  NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return 0.0;

    float3 H     = normalize(L + V);
    float  NdotH = saturate(dot(N, H));

    float a  = max(roughness, 0.045);
    float a2 = a * a; a2 *= a2;

    float Vis  = V_SmithGGXCorrelated(NdotV, NdotL, a2);
    float D    = D_GGX(NdotH, a2);
    float brdf = Vis * D * NdotL;
    float w    = brdf / max(PDF, 1e-5);

    // Lobe alignment — a GENTLE linear bias toward this pixel's mirror
    // reflection. The GGX D term above ALREADY provides sharp lobe weighting
    // (it also peaks at L≈Rmirror), so the original pow(.,2/a) — exponent 20 at
    // roughness 0.1 — double-suppressed off-lobe neighbours and collapsed
    // weightSum on small/curved glossy surfaces straight into the noisy 1-spp
    // self fallback (exactly the noise this pass is meant to remove). Linear
    // keeps a mild directional preference without that collapse.
    w *= saturate(dot(L, Rmirror));

    // Ray-length similarity — a neighbour whose hit sits at a very different
    // distance saw DIFFERENT content; soft-reject via a relative Gaussian.
    // Skipped when this pixel has no valid self hit (selfRayLen ~ 0), so a
    // missed-self pixel can still pull a resolve from its neighbourhood.
    if (selfRayLen > 0.1)
    {
        float rlRel = (npRL - selfRayLen) / max(selfRayLen, 0.1);
        w *= exp(-rlRel * rlRel * 2.0);
    }
    return w;
}

// Vogel golden-angle disk — near-blue-noise sample distribution at zero
// precompute cost. Returns a point in the unit disk; caller scales by radius.
// r uses i/count (not (i+0.5)/count) so sample 0 lands EXACTLY on the center
// pixel — a guaranteed self tap whose importance-sampled hit keeps weightSum
// non-zero even if every off-center neighbour is BRDF-rejected. Without it,
// small/curved glossy surfaces could collapse into the noisy raw-self fallback.
float2 VogelDisk(uint i, uint count, float phi)
{
    const float goldenAngle = 2.39996322973;   // radians, π(3−√5)
    float r     = sqrt(float(i) / float(count));
    float theta = float(i) * goldenAngle + phi;
    return float2(r * cos(theta), r * sin(theta));
}

uint WangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u; s ^= s >> 4u; s *= 0x27d4eb2du; s ^= s >> 15u;
    return s;
}

float2 Hammersley2DRandom(uint i, uint N, uint2 random)
{
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float vdc = float(bits ^ random.y) * 2.3283064365386963e-10;
    return float2(frac(float(i) / float(N) +
                        float(random.x & 0xFFFFu) * (1.0 / 65536.0)),
                  vdc);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    // Phase 7: full-res dispatch — one thread per GBuffer pixel.
    if (DTid.x >= traceW || DTid.y >= traceH) return;

    const int2   pixel = int2(DTid.xy);
    const float2 uv    = (float2(pixel) + 0.5) / float2(renderW, renderH);

    float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        OutColor[pixel]       = 0;
        OutVariance[pixel]    = 0;
        OutReprojDepth[pixel] = 0;
        return;
    }

    float4 surf = gSurface.Load(int3(pixel, 0));
    float  roughness = max(surf.r, 0.045);
    float3 N = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 V = normalize(cameraPos - worldPos);
    float  NdotV = saturate(dot(N, V));
    float  linDepth = LinearizeReverseZ(depth);

    // --- Mirror / near-mirror fast path -----------------------------------
    // For very low roughness the GGX lobe is so tight that neighbour samples'
    // L rarely lie in the current pixel's lobe; GetWeight collapses to zero
    // for every neighbour and the BRDF-weighted loop falls back to self.
    //
    // BUT we can't just use self verbatim — the trace walker is pixel-
    // discrete (1-pixel steps in screen space), so adjacent reflective
    // pixels whose rays differ by sub-pixel amounts can land on integer hit
    // pixels that jump by 1 in either direction. On a smooth surface that
    // would be invisible; at high-contrast edges (column silhouette vs dark
    // gap, lantern body vs background) it shows up as stair-step / striping
    // artefacts in the final reflection. Upsample's variance-gated bilateral
    // blur won't catch it because mirror variance is 0, so the only chance
    // to smooth is HERE. Do a 3×3 depth-aware average across same-surface
    // neighbour hits — preserves silhouettes (sky/different-depth neighbours
    // get 0 weight) but blends out single-pixel walker jumps.
    if (roughness < 0.1)
    {
        float4 acc   = 0;
        float  wSum  = 0;
        float  selfRL = gRayLength.Load(int3(pixel, 0));

        // 5×5 separable-style Gaussian-weighted box across same-surface
        // neighbours. Wider than the previous 3×3 because the walker's
        // staircase / mip-stripe artefacts span 2–4 pixels at high-contrast
        // silhouette edges (right-side column reflections in Sponza were
        // visibly stripped through 3×3). The depth weight is relaxed (32
        // instead of 64) so a slight slope across the floor doesn't kill
        // neighbour contributions and re-tighten the staircase. Pure-sky
        // pixels (sd<=0) are still skipped — they have no plane to align to.
        [unroll] for (int dy = -2; dy <= 2; ++dy)
        [unroll] for (int dx = -2; dx <= 2; ++dx)
        {
            int2 np = clamp(pixel + int2(dx, dy),
                            int2(0, 0),
                            int2(int(renderW) - 1, int(renderH) - 1));
            float4 s  = gHitBuffer.Load(int3(np, 0));
            float  sd = gDepth.Load(int3(np, 0));
            if (sd <= 0.0) continue;
            float linSd  = LinearizeReverseZ(sd);
            float relDz  = abs(linSd - linDepth) / max(linDepth, 0.01);
            float wDepth = exp(-relDz * relDz * 32.0);
            // Gaussian σ≈1.5 spatial weight (kernel sums ≈ 1).
            float r2     = float(dx * dx + dy * dy);
            float wSpat  = exp(-r2 / 4.5);
            float w      = wDepth * wSpat;
            acc  += s * w;
            wSum += w;
        }

        float4 outC = (wSum > 1e-5)
            ? (acc / wSum)
            : gHitBuffer.Load(int3(pixel, 0));
        OutColor[pixel]       = outC;
        OutVariance[pixel]    = 0.0;
        OutReprojDepth[pixel] = InverseLinearDepth(linDepth + selfRL);
        return;
    }

    // Sample radius: glossy [2..8 px] scaled by roughness.
    const float  spatialSize = lerp(2.0, 8.0, saturate(roughness * 5.0));
    // 4 -> 8 taps. 4 single-frame BRDF samples is far too few once temporal
    // accumulation is unavailable (small surfaces whose reprojected history is
    // rejected every frame), so their 1-spp trace noise survives to the output.
    // Denser per-frame sampling cuts that noise ~sqrt(2)x regardless of history
    // or neighbour availability. (The 2026-05-18 tuning ran this at 16; 8 is the
    // conservative restore — bump toward 16 if small-surface noise persists.)
    const uint   kSamples    = 8u;

    // Per-pixel Vogel rotation phase, varied per frame for temporal decorrelation.
    uint  hseed = WangHash(pixel.x + pixel.y * renderW + frameIndex * 0x9E3779B9u);
    float phi   = float(hseed & 0xFFFFu) * (6.28318530718 / 65536.0);

    // References for the parallax-corrected, lobe-aligned neighbour weighting.
    float3 Rmirror    = reflect(-V, N);
    float  selfRayLen = gRayLength.Load(int3(pixel, 0));

    float4 accumColor = 0;
    float  weightSum  = 0;
    float  mean = 0, S = 0;               // Welford weighted variance accumulators
    float  nearestRayLength = 1e9;        // true "closest" (smallest ray length)

    [unroll]
    for (uint i = 0; i < kSamples; ++i)
    {
        // Vogel golden-angle disk: covers the radius more evenly than the old
        // Hammersley-in-square for the same budget, and its residual is
        // higher-frequency (easier for temporal + upsample to mop up).
        float2 offset = VogelDisk(i, kSamples, phi) * spatialSize;
        int2 np = clamp(pixel + int2(offset),
                        int2(0, 0),
                        int2(int(renderW) - 1, int(renderH) - 1));

        float npDepth = gDepth.Load(int3(np, 0));
        if (npDepth <= 0.0) continue;

        float w = GetWeight(np, npDepth, worldPos, V, N, roughness, NdotV,
                            Rmirror, selfRayLen);
        if (w <= 0.0) continue;

        float4 sampleColor = gHitBuffer.Load(int3(np, 0));

        // Karis tone-map round-trip (matches Wicked / FidelityFX-SSSR):
        //   accumulate c' = c / (1 + Y),  Y = Luminance(c)
        //   then expand back: c = c' / (1 - Y'), Y' = Luminance(weighted mean of c')
        //
        // WHY: without this, a single neighbour that hit the sky (HDR
        // luminance can be 100+ from sky/atmosphere/sun) completely
        // dominates the weighted mean. The visible symptom was reflections
        // showing only sky-tinted colour even when 3/4 neighbours hit
        // meshes — the bright sample's raw weighted contribution swamps
        // the others. Karis y/(1+y) maps [0,∞) → [0,1) so all samples
        // contribute on a comparable scale; the inverse expands the mean
        // back to HDR proportionally.
        //
        // Y' < 1 is guaranteed by the forward tonemap (Y/(1+Y) < 1 for any
        // non-negative Y). The safety clamp on (1 - Y') handles only the
        // pathological case where FP precision lands Y' fractionally above
        // 1 — keeps the divisor positive without changing typical values.
        float Yin = max(Luminance(sampleColor.rgb), 0.0);
        float3 toneMapped = sampleColor.rgb * rcp(1.0 + Yin);

        accumColor.rgb += toneMapped * w;
        accumColor.a   += sampleColor.a * w;
        weightSum      += w;

        float lum = Luminance(sampleColor.rgb);
        float oldMean = mean;
        mean += (w / weightSum) * (lum - oldMean);
        S    += w * (lum - oldMean) * (lum - mean);

        if (w > 0.001)
        {
            float rl = gRayLength.Load(int3(np, 0));
            nearestRayLength = min(nearestRayLength, rl);
        }
    }

    float3 colorOut;
    float  confOut;
    if (weightSum > 1e-5)
    {
        accumColor.rgb /= weightSum;
        accumColor.a   /= weightSum;

        // Karis inverse tone-map. Yavg is the mean of (Y/(1+Y)) values which
        // is mathematically < 1; the saturate/0.999 clamp is purely a
        // floating-point safety net for the edge case where many samples'
        // luminance pile against the upper bound and FP rounding nudges the
        // mean to 1.0. Without it a divisor of 0 would produce +Inf which
        // then becomes the "speckled dark spots" after downstream clamps.
        float Yavg     = saturate(Luminance(accumColor.rgb));
        float invScale = rcp(max(1.0 - Yavg, 1e-3));   // cap amplification at 1000×
        accumColor.rgb = accumColor.rgb * invScale;

        // Firefly cap — secondary safety on the expanded HDR. With Karis
        // round-trip already absorbing most of the variance, this only kicks
        // in for true outliers (e.g., specular sun hits). <=0 disables.
        if (fireflyCap > 0.0)
        {
            float y = Luminance(accumColor.rgb);
            if (y > fireflyCap) accumColor.rgb *= fireflyCap / y;
        }
        colorOut = accumColor.rgb;
        confOut  = saturate(accumColor.a);
    }
    else
    {
        // No valid neighbour — fall back to self.
        float4 self = gHitBuffer.Load(int3(pixel, 0));
        colorOut = self.rgb;
        confOut  = self.a;
    }

    OutColor[pixel]    = float4(colorOut, confOut);
    OutVariance[pixel] = (weightSum > 1e-5) ? (S / weightSum) : 0.0;

    // Reprojection depth uses the NEAREST hit from the neighbourhood so the
    // temporal reflection-following reprojection lands on the most visually
    // dominant hit surface (farther hits matter less for reprojection).
    if (nearestRayLength >= 1e9) nearestRayLength = 0.0;
    OutReprojDepth[pixel] = InverseLinearDepth(linDepth + nearestRayLength);
}
