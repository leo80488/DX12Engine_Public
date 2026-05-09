// SSRComposite.cs.hlsl — additive SSR composite + full-screen debug modes.
//
// Pass 6 of the Hi-Z SSR pipeline. Mode 0 is the physically sensible
// additive composite (resolved reflection × Fresnel × envBRDF × conf ×
// intensity, added onto HDR). Every other mode is a full-screen debug
// replacement of HDR so you can see that stage of the pipeline at full
// viewport resolution.
//
// Per-stage texture PREVIEWS (ray length, variance, temporal etc.) live in
// the editor's SSR Debug window as ImGui::Image tiles — those don't need a
// shader at all, they sample the SRVs directly through ImGui's backend.
//
// Debug modes (set via SSRCompositePass::SetDebugMode from the editor):
//   0  Off                     — normal additive composite.
//   1  Final SSR only          — replace HDR with gSSR.rgb * intensity.
//                                gSSR = upsample output; no Fresnel applied.
//   2  Final confidence        — heatmap of gSSR.a (blue=0 → red=1).
//   3  Raw trace hit colour    — gRawTrace.rgb. This is what SSRPass wrote
//                                BEFORE spatial reweight. If this is black
//                                but the final is black too, tracing fails.
//   4  Raw trace confidence    — heatmap of gRawTrace.a. White where trace
//                                found a valid hit, black on miss / early-out.
//   5  Ray direction (world L) — (L*0.5+0.5) as RGB. No ray → pure black.
//                                Sanity check: on a floor, green dominates
//                                (L.y > 0, pointing up).
//   6  Roughness cutoff mask   — green where roughness <= cutoff (SSR runs),
//                                red where rougher (probe fallback only).
//   7  Surface normal          — (N*0.5+0.5) RGB for GBuffer sanity.
//
// Texture bindings stay within t0..t7 (all other compute passes already hit
// the same root-sig slots, so no root-sig changes are needed).

cbuffer SSRCompositeCB : register(b0, space2)
{
    float4x4 invViewProj;
    float3   cameraPos;    float  _pad0;
    uint     screenW;      uint   screenH;
    float    intensity;    uint   debugMode;
    float    roughnessCutoff; float _pad1;
};

Texture2D<float4>   gAlbedo    : register(t0, space2);
Texture2D<float4>   gNormal    : register(t1, space2);
Texture2D<float4>   gSurface   : register(t2, space2);
Texture2D<float>    gDepth     : register(t3, space2);
Texture2D<float4>   gSSR       : register(t4, space2);  // upsample (rgb, conf)
Texture2D<float2>   gBRDFLUT   : register(t5, space2);
Texture2D<float4>   gRawTrace  : register(t6, space2);  // SSRPass hit (rgb, conf)
Texture2D<float4>   gRayDirPDF : register(t7, space2);  // world L (.rgb) + PDF (.a)

RWTexture2D<float4> OutHDR     : register(u0, space2);
SamplerState        gLinClamp  : register(s0, space2);

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc;
    ndc.x =  uv.x * 2.0 - 1.0;
    ndc.y =  1.0 - uv.y * 2.0;
    float4 clip = float4(ndc, depth, 1.0);
    float4 wp = mul(clip, invViewProj);
    return wp.xyz / wp.w;
}

float3 FresnelSchlickRoughness(float NdotV, float3 F0, float roughness)
{
    float3 Fr = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0;
    return F0 + Fr * pow(saturate(1.0 - NdotV), 5.0);
}

// Blue → cyan → green → yellow → red visualisation. Input clamped to [0,1].
float3 Heatmap01(float x)
{
    x = saturate(x);
    return float3(
        saturate(1.5 - abs(4.0 * x - 3.0)),
        saturate(1.5 - abs(4.0 * x - 2.0)),
        saturate(1.5 - abs(4.0 * x - 1.0)));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= screenW || DTid.y >= screenH) return;
    const int2 pixel = int2(DTid.xy);

    // ========================================================================
    // Debug branches — every non-zero mode fully overwrites HDR so the screen
    // shows only that debug view, without leakage from prior passes.
    // ========================================================================
    if (debugMode != 0)
    {
        float3 outC = 0;
        switch (debugMode)
        {
        case 1:  // Final SSR only — multiplied by intensity slider so the user
                 // can verify via the debug panel whether the upsample is
                 // truly zero vs just tonemap-dim. At intensity=50 any
                 // non-zero HDR value should be visible on screen.
            outC = gSSR.Load(int3(pixel, 0)).rgb * intensity;
            break;

        case 2:  // Final confidence heatmap
            outC = Heatmap01(saturate(gSSR.Load(int3(pixel, 0)).a));
            break;

        case 3:  // Raw trace hit colour — pure texture read, zero scaling.
                 // If this is black while mode 4 (conf) shows hits, hitColor
                 // in SSRTrace.cs.hlsl sampled a zero pyramid texel.
            outC = gRawTrace.Load(int3(pixel, 0)).rgb;
            break;

        case 4:  // Raw trace confidence heatmap
            outC = Heatmap01(saturate(gRawTrace.Load(int3(pixel, 0)).a));
            break;

        case 5:  // Ray direction (world L)
        {
            float3 L = gRayDirPDF.Load(int3(pixel, 0)).rgb;
            // Miss → pure black so you can see which pixels got no ray.
            outC = (dot(L, L) < 1e-6) ? 0.0 : (L * 0.5 + 0.5);
            break;
        }

        case 6:  // Roughness cutoff mask
        {
            float d = gDepth.Load(int3(pixel, 0));
            if (d <= 0.0) { outC = 0; break; }
            float r = gSurface.Load(int3(pixel, 0)).r;
            float3 tint = (r <= roughnessCutoff) ? float3(0.15, 0.75, 0.15)
                                                 : float3(0.75, 0.15, 0.15);
            outC = tint * (1.0 - r * 0.6);
            break;
        }

        case 7:  // Surface normal
        {
            float3 N = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
            outC = N * 0.5 + 0.5;
            break;
        }

        case 8:  // SANITY — write pure white regardless of any input. If this
                 // mode shows white, composite is running and the HDR UAV
                 // write path is fine; any failure to show content in modes
                 // 1/3 is upstream (SSR chain is producing zeros).
            outC = float3(1.0, 1.0, 1.0);
            break;

        case 9:  // SSR (upsample) presence mask
        {
            float3 rgb = gSSR.Load(int3(pixel, 0)).rgb;
            bool any = (rgb.r > 1e-8) || (rgb.g > 1e-8) || (rgb.b > 1e-8);
            outC = any ? float3(0.0, 0.9, 0.1) : float3(0.9, 0.1, 0.1);
            break;
        }

        case 10:  // Raw trace presence — upstream of resolve/temporal/upsample.
                  // If this is all green, trace IS producing content and the
                  // zero in upsample means resolve/temporal wiped it.
        {
            float3 rgb = gRawTrace.Load(int3(pixel, 0)).rgb;
            bool any = (rgb.r > 1e-8) || (rgb.g > 1e-8) || (rgb.b > 1e-8);
            outC = any ? float3(0.0, 0.9, 0.1) : float3(0.9, 0.1, 0.1);
            break;
        }

        case 11:  // Ray-emission mask — green if trace issued any ray on this
                  // pixel (L direction written). Further upstream than hit
                  // colour; tells you if the shader even entered the trace
                  // path (vs early-exit on roughness / sky depth / etc.).
        {
            float3 L = gRayDirPDF.Load(int3(pixel, 0)).rgb;
            bool any = dot(L, L) > 1e-8;
            outC = any ? float3(0.0, 0.9, 0.1) : float3(0.9, 0.1, 0.1);
            break;
        }

        default: break;
        }
        OutHDR[pixel] = float4(outC, 1.0);
        return;
    }

    // ========================================================================
    // Mode 0 — normal additive composite.
    // ========================================================================
    const float4 ssr  = gSSR.Load(int3(pixel, 0));
    const float  conf = saturate(ssr.a);
    if (conf <= 1e-4) return;

    const float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0) return;

    const float2 uv = (float2(pixel) + 0.5) / float2(screenW, screenH);
    const float3 wp = ReconstructWorldPos(uv, depth);
    const float3 N  = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    const float3 V  = normalize(cameraPos - wp);
    const float  NdotV = saturate(dot(N, V));

    const float3 albedo  = gAlbedo.Load(int3(pixel, 0)).rgb;
    const float4 surf    = gSurface.Load(int3(pixel, 0));
    const float  rough   = max(surf.r, 0.045);
    const float  metal   = surf.g;
    const float  refl    = surf.a;

    const float  f0s = 0.16 * refl * refl;
    const float3 F0  = lerp(float3(f0s, f0s, f0s), albedo, metal);
    const float3 F   = FresnelSchlickRoughness(NdotV, F0, rough);

    const float2 envBRDF  = gBRDFLUT.SampleLevel(gLinClamp, float2(NdotV, rough), 0);
    const float3 specTerm = F * envBRDF.x + envBRDF.y;

    const float3 reflection = ssr.rgb * specTerm * conf * intensity;

    const float4 hdr = OutHDR[pixel];
    OutHDR[pixel] = float4(hdr.rgb + reflection, hdr.a);
}
