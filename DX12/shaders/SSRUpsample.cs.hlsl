// SSRUpsample.cs.hlsl — variance-driven bilateral blur (final SSR stage).
//
// Pass 5 of the Hi-Z SSR pipeline. Ported from Wicked's ssr_upsampleCS.
// Reads the temporal-filtered color + per-pixel variance; in regions where
// variance is high, runs a separable (H then V) bilateral blur whose radius
// and weights are modulated by depth/normal similarity. Regions with low
// variance (clean mirrors, well-converged history) pass through untouched.
//
// Output (RGBA16F): final SSR color that LightingPass reads (1-frame latent)
// and SSRComposite additively blends into HDR.

cbuffer SSRUpsampleCB : register(b0, space2)
{
    float4x4 invViewProj;
    uint     screenW;      uint   screenH;
    float    invScreenW;   float  invScreenH;
    float    nearZ;        float  farZ;
    float    _pad0;        float  _pad1;
};

Texture2D<float4>  gTemporal   : register(t0, space2);  // temporal color
Texture2D<float>   gVariance   : register(t1, space2);  // temporal / resolve variance
Texture2D<float>   gDepth      : register(t2, space2);
Texture2D<float4>  gNormal     : register(t3, space2);
Texture2D<float4>  gSurface    : register(t4, space2);  // roughness in .r

RWTexture2D<float4> OutColor   : register(u0, space2);

SamplerState gLinClamp : register(s0, space2);

static const float kDepthThreshold    = 10000.0;
static const float kNormalThreshold   = 1.0;
static const float kVarianceEstimate  = 0.015;   // high-variance gate
static const float kVarianceExit      = 0.005;   // min variance to blur at all
static const int   kMinRadius         = 0;
static const int   kMaxRadius         = 2;
static const float kBilateralSigma    = 0.9;

// ---------------------------------------------------------------------------
float LinearizeReverseZ(float zNdc)
{
    return (nearZ * farZ) / (nearZ + (farZ - nearZ) * zNdc);
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp = mul(float4(ndc, depth, 1.0), invViewProj);
    return wp.xyz / wp.w;
}

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= screenW || DTid.y >= screenH) return;
    const int2   pixel = int2(DTid.xy);
    const float2 uv    = (float2(pixel) + 0.5) * float2(invScreenW, invScreenH);

    float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0) { OutColor[pixel] = 0; return; }

    float  roughness = max(gSurface.Load(int3(pixel, 0)).r, 0.045);
    float3 N         = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);

    float4 outputColor = gTemporal.SampleLevel(gLinClamp, uv, 0);
    float  variance    = gVariance.SampleLevel(gLinClamp, uv, 0);

    // Variance-driven radius. Strong blur on high-variance taps; none on clean.
    bool strongBlur = variance > kVarianceEstimate;
    float radiusF = strongBlur ? float(kMaxRadius) : float(kMinRadius);
    radiusF = lerp(0.0, radiusF, saturate(roughness * 8.0));   // roughness 0.125 = full

    float sigma = radiusF * kBilateralSigma;
    int effRadius = min((int)(sigma * 2.0), (int)radiusF);

    [branch]
    if (variance > kVarianceExit && effRadius > 0)
    {
        float3 P = ReconstructWorldPos(uv, depth);
        float  linDepth = LinearizeReverseZ(depth);

        float4 result = 0;
        float  wSum = 0;

        // Separable — H then V in one dispatch (matches Wicked).
        [unroll] for (uint d = 0; d < 2; ++d)
        {
            const int2 dir = d == 0 ? int2(1, 0) : int2(0, 1);
            [loop] for (int r = -effRadius; r <= effRadius; ++r)
            {
                int2 np = pixel + dir * r;
                if (any(np < 0) || np.x >= (int)screenW || np.y >= (int)screenH) continue;

                float sDepth = gDepth.Load(int3(np, 0));
                if (sDepth <= 0.0) continue;

                float2 sUV = (float2(np) + 0.5) * float2(invScreenW, invScreenH);
                float3 sP  = ReconstructWorldPos(sUV, sDepth);
                float3 sN  = normalize(gNormal.Load(int3(np, 0)).rgb * 2.0 - 1.0);

                // Plane-aligned depth error (both surface and sample normals).
                float3 dq = P - sP;
                float  planeErr = max(abs(dot(dq, sN)), abs(dot(dq, N)));
                float  depthRel = planeErr / max(linDepth * farZ, 1e-3);
                float  bDepth   = exp(-depthRel * depthRel * kDepthThreshold);

                float  nErr    = pow(saturate(dot(sN, N)), 4.0);
                float  bNormal = saturate(1.0 - (1.0 - nErr) * kNormalThreshold);

                float  g = exp(-(float)(r * r) / max(sigma * sigma, 1e-5));
                float  w = (r == 0) ? 1.0 : g * bDepth * bNormal;

                result += gTemporal.SampleLevel(gLinClamp, sUV, 0) * w;
                wSum   += w;
            }
        }

        if (wSum > 1e-5) outputColor = result / wSum;
    }

    OutColor[pixel] = outputColor;
}
