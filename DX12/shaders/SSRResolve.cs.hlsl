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
    uint     screenW;     uint   screenH;
    float    invScreenW;  float  invScreenH;
    float    nearZ;       float  farZ;
    uint     frameIndex;  uint   _pad1;
    // Post-accumulation luminance cap (anti-firefly). Prior builds hardcoded
    // 16 which clipped legitimate bright speculars; keep it runtime-tunable.
    float    fireflyCap;  float  _pad3;
};

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

// Current-pixel BRDF weight using the neighbour's sampled (L, PDF).
float GetWeight(int2 np, float3 V, float3 N, float roughness, float NdotV)
{
    float4 lpdf = gRayDirPDF.Load(int3(np, 0));
    if (lpdf.w <= 0.0 || dot(lpdf.xyz, lpdf.xyz) < 1e-6) return 0.0;

    float3 L = normalize(lpdf.xyz);
    float  PDF = lpdf.w;

    float3 H = normalize(L + V);
    float  NdotH = saturate(dot(N, H));
    float  NdotL = saturate(dot(N, L));

    float a  = max(roughness, 0.045);
    float a2 = a * a; a2 *= a2;

    float Vis  = V_SmithGGXCorrelated(NdotV, NdotL, a2);
    float D    = D_GGX(NdotH, a2);
    float brdf = Vis * D * NdotL;
    return brdf / max(PDF, 1e-5);
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
    if (DTid.x >= screenW || DTid.y >= screenH) return;
    const int2   pixel = int2(DTid.xy);
    const float2 uv    = (float2(pixel) + 0.5) * float2(invScreenW, invScreenH);

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
    // for every neighbour and the loop falls back to self. Skip the loop
    // entirely — just re-use our own trace result.
    if (roughness < 0.1)
    {
        float4 self = gHitBuffer.Load(int3(pixel, 0));
        float  selfRL = gRayLength.Load(int3(pixel, 0));
        OutColor[pixel]       = self;
        OutVariance[pixel]    = 0.0;
        OutReprojDepth[pixel] = InverseLinearDepth(linDepth + selfRL);
        return;
    }

    // Sample radius: glossy [2..8px] scaled by roughness.
    const float  spatialSize = lerp(2.0, 8.0, saturate(roughness * 5.0));
    const uint   kSamples    = 4u;

    uint h = WangHash(pixel.x + pixel.y * screenW + frameIndex * 0x9E3779B9u);
    uint2 random = uint2(h & 0xFFFFu, (h >> 16) & 0xFFFFu);

    float4 accumColor = 0;
    float  weightSum  = 0;
    float  mean = 0, S = 0;               // Welford weighted variance accumulators
    float  nearestRayLength = 1e9;        // true "closest" (smallest ray length)

    [unroll]
    for (uint i = 0; i < kSamples; ++i)
    {
        float2 offset = (Hammersley2DRandom(i, kSamples, random) - 0.5) * spatialSize;
        int2   np = clamp(pixel + int2(offset),
                          int2(0, 0), int2(screenW - 1, screenH - 1));

        if (gDepth.Load(int3(np, 0)) <= 0.0) continue;

        float w = GetWeight(np, V, N, roughness, NdotV);
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
        // No valid neighbour — fall back to self so rough surfaces at least
        // show their own hit (better than a black hole).
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
