// LensFlare.cs.hlsl
// Procedural directional-light lens flare. Writes RGBA16F additive contribution
// at half resolution; ToneMap.cs.hlsl samples this texture as g_lensFlare and
// adds it to the HDR scene before tonemapping.
//
// Compute root signature (space2):
//   [0] cbuffer PerDispatch b0 space2
//   [1] SRV t0 space2  — hardware depth (reverse-Z; 0 = far/sky, 1 = near)
//   [2] SRV t1 space2  — cloud raymarch (a = view-ray transmittance)
//   [4] UAV u0 space2  — output flare RGBA16F (overwrite)

cbuffer PerDispatch : register(b0, space2)
{
    uint  g_dstWidth;
    uint  g_dstHeight;
    uint  g_srcDepthWidth;
    uint  g_srcDepthHeight;

    uint  g_enabled;          // 1 = generate flare; 0 = clear to black
    float g_intensity;        // global multiplier
    float g_sunBehind;        // 1 = sun behind camera; skip
    float g_chromaticOffset;  // UV space offset between R/B per ghost

    float2 g_sunUV;           // sun in screen-space UV (can be slightly outside 0..1)
    float  g_haloWidth;       // 0..0.5 (fraction of screen along Y)
    float  g_streakLength;    // 0..1, anamorphic horizontal streak length

    float3 g_sunColor;        // raw sun color (already brightness scaled)
    float  g_ghostDispersal;  // 0..1 (ghost spacing, fraction of sun→center)

    uint   g_ghostCount;      // 0..8
    float  g_streakWidth;     // vertical thickness of streak
    float  g_occlusionRadius; // UV radius for depth taps
    uint   g_cloudValid;      // 1 = g_cloud bound and rendered this frame
};

Texture2D<float>     g_depth  : register(t0, space2);
Texture2D<float4>    g_cloud  : register(t1, space2);
RWTexture2D<float4>  g_output : register(u0, space2);
SamplerState         g_linear : register(s0, space2);

// Smoothstep falloff disc: 1 at center → 0 at radius.
float SoftDisc(float2 uv, float2 center, float radius)
{
    float d = distance(uv, center);
    float t = saturate(1.0f - d / max(radius, 1e-5f));
    return t * t * (3.0f - 2.0f * t);
}

// Halo ring: peak at `radius` from center, falls to 0 at `radius +/- thickness`.
// Smoothstep'd falloff — the previous linear knee produced a hard "donut"
// edge that read as solid/opaque rather than as a soft glow.
float Halo(float2 uv, float2 center, float radius, float thickness)
{
    float d = distance(uv, center);
    float t = saturate(1.0f - abs(d - radius) / max(thickness, 1e-5f));
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); // quintic smoothstep
}

// Sample depth at sunUV with N taps in a small disc; returns 0..1 occlusion
// (1 = fully un-occluded sky, 0 = fully occluded by geometry).
// Reverse-Z: depth=0 is far plane (sky); anything > epsilon means geometry blocks the sun.
float SampleSunOcclusion(float2 sunUV)
{
    // Sun barely on screen → no depth available; treat as fully visible.
    if (any(sunUV < float2(0.0f, 0.0f)) || any(sunUV > float2(1.0f, 1.0f)))
        return 1.0f;

    const int   kTaps   = 8;
    const float kThresh = 1e-4f;       // depth above this = geometry in the way
    float visible = 0.0f;
    visible += (g_depth.SampleLevel(g_linear, sunUV, 0).r <= kThresh) ? 1.0f : 0.0f;

    [unroll]
    for (int i = 0; i < kTaps; ++i)
    {
        float ang = 6.2831853f * (float(i) + 0.5f) / float(kTaps);
        float2 off = float2(cos(ang), sin(ang)) * g_occlusionRadius;
        float2 uv  = saturate(sunUV + off);
        float  d   = g_depth.SampleLevel(g_linear, uv, 0).r;
        visible += (d <= kThresh) ? 1.0f : 0.0f;
    }
    return visible / float(kTaps + 1);
}

// Volumetric clouds never write depth, so the depth test above sees clear
// sky through full overcast. Sample the cloud raymarch's transmittance
// (alpha; 1 = clear, 0 = opaque cloud) in the same small disc instead —
// continuous, so the flare fades smoothly as a cloud edge crosses the sun.
float SampleCloudTransmittance(float2 sunUV)
{
    if (g_cloudValid == 0u) return 1.0f;
    if (any(sunUV < float2(0.0f, 0.0f)) || any(sunUV > float2(1.0f, 1.0f)))
        return 1.0f;

    const int kTaps = 8;
    float tr = g_cloud.SampleLevel(g_linear, sunUV, 0).a;
    [unroll]
    for (int i = 0; i < kTaps; ++i)
    {
        float ang = 6.2831853f * (float(i) + 0.5f) / float(kTaps);
        float2 uv = saturate(sunUV + float2(cos(ang), sin(ang))
                                     * (g_occlusionRadius * 2.0f));
        tr += g_cloud.SampleLevel(g_linear, uv, 0).a;
    }
    return tr / float(kTaps + 1);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstWidth || id.y >= g_dstHeight) return;

    if (g_enabled == 0 || g_sunBehind > 0.5f)
    {
        g_output[id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float2 uv = (float2(id.xy) + 0.5f) / float2(g_dstWidth, g_dstHeight);

    // Aspect-correct UV (so circles stay circular and ghost spacing is even).
    float aspect = float(g_dstWidth) / max(1.0f, float(g_dstHeight));
    float2 uvAR  = float2((uv.x      - 0.5f) * aspect + 0.5f, uv.y);
    float2 sunAR = float2((g_sunUV.x - 0.5f) * aspect + 0.5f, g_sunUV.y);
    float2 ctrAR = float2(0.5f, 0.5f);

    // Sun on-screen edge fade (over 10% of screen border).
    float2 edge = min(g_sunUV, 1.0f - g_sunUV);
    float onScreen = saturate(min(edge.x, edge.y) / 0.10f + 0.05f);

    // Depth-based occlusion (computed once per pixel — cheap, fully unrolled
    // tap loop), attenuated by cloud transmittance at the sun.
    float occ = SampleSunOcclusion(g_sunUV) * SampleCloudTransmittance(g_sunUV);

    float intensity = g_intensity * onScreen * occ;
    if (intensity <= 1e-4f)
    {
        g_output[id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 color = float3(0.0f, 0.0f, 0.0f);

    // ---- Sun core glow ----
    // Tight, bright disc + a soft, low-intensity falloff. Outer glow used
    // to be 1.5x which read as a flat opaque blob; 0.6x lets the underlying
    // HDR scene show through and reads as light scatter rather than paint.
    color += g_sunColor * SoftDisc(uvAR, sunAR, 0.045f) * 5.0f;
    color += g_sunColor * SoftDisc(uvAR, sunAR, 0.13f)  * 0.55f;

    // ---- Halo ring ----
    // Wider, softer, dimmer than before. The smoothstep'd Halo() makes the
    // edge fall off gradually so it doesn't draw as a printed circle.
    float halo = Halo(uvAR, sunAR, g_haloWidth, g_haloWidth * 0.6f);
    color += g_sunColor * halo * 0.18f;

    // ---- Anamorphic horizontal streak ----
    float dx = uvAR.x - sunAR.x;
    float dy = uvAR.y - sunAR.y;
    float streakBody = exp(-(dy * dy) / max(1e-5f, g_streakWidth * g_streakWidth));
    float streakFade = saturate(1.0f - abs(dx) / max(1e-4f, g_streakLength));
    float streak     = streakBody * streakFade;
    color += float3(0.55f, 0.75f, 1.20f) * g_sunColor * streak * 1.2f;

    // ---- Ghosts: along the line from sun → screen center ----
    float2 sunToCenter   = float2(0.5f, 0.5f) - g_sunUV;
    float2 sunToCenterAR = float2(sunToCenter.x * aspect, sunToCenter.y);
    float  dispLen       = length(sunToCenterAR) + 1e-5f;
    float2 ghostDir      = sunToCenterAR / dispLen;

    [unroll(8)]
    for (uint i = 0u; i < g_ghostCount; ++i)
    {
        float t = (float(i) + 1.0f) * g_ghostDispersal;
        // Walk from sun across center and beyond.
        float2 ghost      = g_sunUV       + sunToCenter   * (t * 2.0f);
        float2 ghostAR    = float2((ghost.x - 0.5f) * aspect + 0.5f, ghost.y);

        float radius = lerp(0.020f, 0.075f, frac(float(i) * 0.371f + 0.13f));
        float chrom  = g_chromaticOffset * (1.0f + float(i) * 0.5f);

        float dR = SoftDisc(uvAR, ghostAR + ghostDir * chrom, radius);
        float dG = SoftDisc(uvAR, ghostAR,                    radius);
        float dB = SoftDisc(uvAR, ghostAR - ghostDir * chrom, radius);

        // Fade ghosts that wander far past the screen so the chain doesn't
        // produce a single bright dot at the canvas corner.
        float screenFade = 1.0f - saturate((distance(ghostAR, ctrAR) - 0.7f) / 0.3f);

        float3 tint = lerp(g_sunColor, float3(0.6f, 0.8f, 1.0f),
                           frac(float(i) * 0.293f + 0.1f));
        color += float3(dR, dG, dB) * tint * 0.45f * screenFade;
    }

    color *= intensity;

    g_output[id.xy] = float4(color, 1.0f);
}
