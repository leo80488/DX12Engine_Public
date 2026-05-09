// SSRTemporal.cs.hlsl — history reprojection + neighborhood clipping.
//
// Pass 4 of the Hi-Z SSR pipeline, split out from resolve per Wicked's
// ssr_temporalCS. Two reprojection candidates compete:
//   (a) velocity-based prev UV  — correct for surface motion
//   (b) reflection-hit prev UV  — runs the hit point through prev ViewProj,
//                                 correct for dynamic reflected content
// The one whose history sample's luminance is closer to the neighbourhood
// mean wins. History is then clamped to a YCoCg AABB (Welford 3×3) to
// suppress ghosting.
//
// Outputs: temporal color + temporal variance (ping-pong with prev frame).

cbuffer SSRTemporalCB : register(b0, space2)
{
    // BOTH matrices are jittered:
    //   invViewProj  = inverse of CURRENT-frame jittered VP. Unprojecting
    //                  (uv, depth) with the same matrix that produced the
    //                  rasterized depth recovers the exact surface world
    //                  position (no sub-pixel slop).
    //   prevViewProj = PREVIOUS-frame jittered VP. Projecting world →
    //                  prev NDC lands UVs on the actual prev jittered
    //                  pixel grid — where the history texture lives.
    // The earlier code paired jittered invVP with NON-jittered prevVP,
    // which placed the history sample ~prevJitter (~1 px) off every frame.
    // The EMA at 0.95 dragged that into a multi-pixel ghost trail and a
    // perceptible "reflection misaligned" look even on static frames.
    float4x4 invViewProj;           // current inverse (jittered)
    float4x4 prevViewProj;          // previous-frame viewProj (jittered)
    uint     screenW;    uint   screenH;
    float    invScreenW; float  invScreenH;
    uint     resetHistory;          // 1 = skip history read (frame 0 / resize)
    float    nearZ;      float  farZ;
    float    _pad0;
};

Texture2D<float4>  gColorCurrent    : register(t0, space2);  // resolve color
Texture2D<float4>  gColorHistory    : register(t1, space2);  // prev-frame temporal
Texture2D<float>   gVarianceCurrent : register(t2, space2);  // resolve variance
Texture2D<float>   gVarianceHistory : register(t3, space2);  // prev-frame temporal variance
Texture2D<float>   gReprojDepth     : register(t4, space2);  // from resolve
Texture2D<float2>  gVelocity        : register(t5, space2);  // NDC delta
Texture2D<float>   gDepth           : register(t6, space2);  // current depth
Texture2D<float>   gDepthHistory    : register(t7, space2);  // prev-frame depth

RWTexture2D<float4> OutColor        : register(u0, space2);
RWTexture2D<float>  OutVariance     : register(u1, space2);
// Updated every frame with the CURRENT pixel's reverse-Z NDC depth so the
// next frame's gDepthHistory (which is this UAV's ping-pong partner read as
// SRV) carries a real value. Without this, gDepthHistory was zero, every
// pixel's disocclusion test failed, history was discarded, and temporal
// accumulation never converged — variance stayed maxed and the upsample
// blurred everything as if it had just disoccluded.
RWTexture2D<float>  OutDepthHistory : register(u2, space2);

SamplerState gLinClamp : register(s0, space2);

static const float kTemporalResponse  = 0.85;
static const float kTemporalScale     = 1.0;    // σ multiplier
static const float kDisoccWeight      = 2.0;    // depth rejection sharpness
static const float kDisoccThreshold   = 0.3;
static const float kVarianceResponse  = 0.9;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
float LinearizeReverseZ(float zNdc)
{
    return (nearZ * farZ) / (nearZ + (farZ - nearZ) * zNdc);
}

float3 RGBToYCoCg(float3 c)
{
    return float3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                   0.5  * c.r              - 0.5  * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 YCoCgToRGB(float3 c)
{
    float y = c.x, co = c.y, cg = c.z;
    return float3(y + co - cg, y + cg, y - co - cg);
}

float3 ClipAABB(float3 center, float3 color, float3 minC, float3 maxC)
{
    float3 e = 0.5 * (maxC - minC) + 1e-5;
    float3 v = color - center;
    float3 a = abs(v / e);
    float  m = max(a.x, max(a.y, a.z));
    return (m > 1.0) ? (center + v / m) : color;
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

bool InRange01(float2 uv) { return all(uv > 0.0) && all(uv < 1.0); }

// Reflection-hit reprojection: unproject (uv, reprojDepth) through prev VP.
// Both invViewProj and prevViewProj are jittered — see CB comment block.
float2 ReflectionReproject(float2 uv, float reprojDepth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 clip = float4(ndc, reprojDepth, 1.0);
    float4 wp   = mul(clip, invViewProj);
    wp /= wp.w;

    float4 prevClip = mul(float4(wp.xyz, 1.0), prevViewProj);
    if (prevClip.w <= 1e-3) return uv;
    float2 prevNdc = prevClip.xy / prevClip.w;
    return float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

float GetDisocclusion(float curLinZ, float prevLinZ)
{
    float rel = abs(curLinZ - prevLinZ) / max(curLinZ, 0.01);
    return exp(-rel * kDisoccWeight);
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

    float4 current  = gColorCurrent.Load(int3(pixel, 0));
    float  curVar   = gVarianceCurrent.Load(int3(pixel, 0));
    float  depth    = gDepth.Load(int3(pixel, 0));

    // ALWAYS update depth history before any early-out — sky pixels and
    // history-reset pixels still have valid GBuffer depth, and next frame's
    // disocclusion test needs every texel populated (otherwise neighbouring
    // pixels read zero and the 3×3 search fallback can't find a valid match).
    OutDepthHistory[pixel] = depth;

    // History reset — frame 0 or after resize.
    if (resetHistory != 0 || depth <= 0.0)
    {
        OutColor[pixel]    = current;
        OutVariance[pixel] = curVar;
        return;
    }

    float curLinZ = LinearizeReverseZ(depth);

    // Welford 3×3 neighbourhood for YCoCg clip.
    float3 m1 = 0, m2 = 0;
    int    n  = 0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        int2 np = clamp(pixel + int2(dx, dy),
                        int2(0, 0), int2(screenW - 1, screenH - 1));
        float4 s = gColorCurrent.Load(int3(np, 0));
        float3 c = RGBToYCoCg(s.rgb);
        m1 += c; m2 += c * c; n++;
    }
    float invN = 1.0 / float(n);
    float3 mean3  = m1 * invN;
    float3 stdev3 = sqrt(abs(m2 * invN - mean3 * mean3));
    float3 minYC = mean3 - kTemporalScale * stdev3;
    float3 maxYC = mean3 + kTemporalScale * stdev3;

    // Dual reprojection candidates.
    float2 motionNDC = gVelocity.Load(int3(pixel, 0));
    float2 motionUV  = float2(0.5, -0.5) * motionNDC;
    float2 prevUV_v  = uv - motionUV;

    float reprojDepth = gReprojDepth.Load(int3(pixel, 0));
    float2 prevUV_r  = ReflectionReproject(uv, reprojDepth);

    float4 histV = gColorHistory.SampleLevel(gLinClamp, prevUV_v, 0);
    float4 histR = gColorHistory.SampleLevel(gLinClamp, prevUV_r, 0);

    float curLum = Luminance(current.rgb);
    float dV = abs(Luminance(histV.rgb) - curLum);
    float dR = abs(Luminance(histR.rgb) - curLum);
    float2 prevUV = (dV < dR && InRange01(prevUV_v)) ? prevUV_v : prevUV_r;
    float4 history = (dV < dR && InRange01(prevUV_v)) ? histV    : histR;

    // Disocclusion via depth history; fallback search if too-low confidence.
    float prevLinZ = LinearizeReverseZ(
        gDepthHistory.SampleLevel(gLinClamp, prevUV, 0));
    float disocc = GetDisocclusion(curLinZ, prevLinZ);

    if (disocc < kDisoccThreshold && InRange01(prevUV))
    {
        // Search 3×3 for the tap whose depth matches best.
        float2 dudv = float2(invScreenW, invScreenH);
        float bestDisocc = disocc;
        float2 bestUV    = prevUV;
        [unroll] for (int sy = -1; sy <= 1; ++sy)
        [unroll] for (int sx = -1; sx <= 1; ++sx)
        {
            float2 testUV = prevUV + float2(sx, sy) * dudv;
            float  tLinZ  = LinearizeReverseZ(
                gDepthHistory.SampleLevel(gLinClamp, testUV, 0));
            float  tDisocc = GetDisocclusion(curLinZ, tLinZ);
            if (tDisocc > bestDisocc) { bestDisocc = tDisocc; bestUV = testUV; }
        }
        prevUV  = bestUV;
        disocc  = bestDisocc;
        history = gColorHistory.SampleLevel(gLinClamp, prevUV, 0);
    }

    float4 result;
    float  resultVar;

    if (disocc >= kDisoccThreshold && InRange01(prevUV))
    {
        // YCoCg AABB clip on history.
        float3 histYC = RGBToYCoCg(history.rgb);
        histYC = ClipAABB(mean3, histYC, minYC, maxYC);
        history.rgb = YCoCgToRGB(histYC);

        result = lerp(current, history, kTemporalResponse);

        float prevVar = gVarianceHistory.SampleLevel(gLinClamp, prevUV, 0);
        resultVar = lerp(curVar, prevVar, kVarianceResponse);
    }
    else
    {
        // Disocclusion: no history, max variance to trigger upsample blur.
        result    = current;
        resultVar = 1.0;
    }

    OutColor[pixel]    = max(result, 0.0);
    OutVariance[pixel] = max(resultVar, 0.0);
}
