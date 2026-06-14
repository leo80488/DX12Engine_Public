// CloudComposite.ps.hlsl
// -----------------------------------------------------------------------------
// Depth-aware upsample of the quarter-resolution cloud raymarch, emitted in
// premultiplied form (rgb = pre-multiplied scatter, a = transmittance).
//
// Blend state set on the PSO is:
//   out = src.rgb + dst.rgb * src.a
//        = scatter + sceneRgb * transmittance
// so a transmittance of 1.0 (clear sky) leaves the scene untouched, and
// transmittance of 0.0 fully replaces the scene with cloud scatter.
//
// Plain bilinear upsample bled cloud across terrain silhouettes (a bright
// outline tracing the terrain edge): a quarter texel straddling the edge is
// one all-or-nothing march, and bilinear smears it onto neighbouring
// full-res pixels of the WRONG classification. Here each full-res pixel
// re-weights the 4 nearest quarter texels by how close the scene distance
// each texel marched against (CloudDist, written by the raymarch) is to
// this pixel's own occluder distance — sky pixels take sky-marched texels,
// terrain pixels take terrain-clamped ones. A shell-entry guard on top
// guarantees zero cloud on any surface in front of the layer.
//
// Graphics root signature only (no space2): CB at b3 space0, depth at t5
// (the GBuffer depth slot), cloud at t6 (re-purposed env-cube slot, same as
// VolumetricFog's apply), march distance at t7.
// -----------------------------------------------------------------------------

#define CLOUD_CB_REGISTER register(b3, space0)
#include "CloudCommon.hlsli"

Texture2D<float>   SceneDepth   : register(t5, space0);
Texture2D<float4>  CloudHalfRes : register(t6, space0);
Texture2D<float>   CloudDist    : register(t7, space0);
SamplerState       LinearClamp  : register(s0, space0);

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target
{
    float2 ndc = float2(i.uv.x * 2.0 - 1.0, -(i.uv.y * 2.0 - 1.0));

    // This pixel's occluder distance — same reconstruction the raymarch
    // used, sky mapped to the shared sentinel.
    float rawZ = SceneDepth.Load(int3(int2(i.pos.xy), 0));
    float sceneDist = kCloudSkyDist;
    if (rawZ > 0.0)   // reversed-Z: 0 = sky
    {
        float4 surfW = mul(float4(ndc, rawZ, 1.0), invViewProj);
        sceneDist = length(surfW.xyz / surfW.w - cameraPos);
    }

    // Hard guard: a surface in front of the shell entry can never sit behind
    // cloud — emit the exact blend no-op. 1 m tolerance avoids shimmer on
    // geometry exactly at the boundary; tEntry = 0 when the camera is inside
    // the layer, so the guard never falsely culls there.
    {
        float4 nearW = mul(float4(ndc, 1.0, 1.0), invViewProj);
        float4 farW  = mul(float4(ndc, 0.0, 1.0), invViewProj);
        float3 rayP  = nearW.xyz / nearW.w;
        float3 rayD  = normalize(farW.xyz / farW.w - rayP);
        float tEntry, tExit;
        if (!ShellInterval(cameraPos, rayD, tEntry, tExit)
            || sceneDist < tEntry - 1.0)
            return float4(0.0, 0.0, 0.0, 1.0);
    }

    // Depth-aware bilateral 4-tap upsample.
    float2 qSize = float2(halfResW, halfResH);
    float2 qf    = i.uv * qSize - 0.5;
    float2 qb    = floor(qf);
    float2 fw    = qf - qb;

    const float2 offs[4] = { float2(0, 0), float2(1, 0),
                             float2(0, 1), float2(1, 1) };
    float wBil[4] = { (1.0 - fw.x) * (1.0 - fw.y), fw.x * (1.0 - fw.y),
                      (1.0 - fw.x) * fw.y,         fw.x * fw.y };

    float4 sum        = 0.0;
    float  wSum       = 0.0;
    float4 nearest    = float4(0.0, 0.0, 0.0, 1.0);
    float  nearestErr = 1e30;

    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        int2 qc = int2(clamp(qb + offs[k], 0.0, qSize - 1.0));
        float4 c = CloudHalfRes.Load(int3(qc, 0));
        float  d = CloudDist.Load(int3(qc, 0));

        // Relative mismatch between the distance that texel marched against
        // and this pixel's occluder. Sky-vs-terrain → huge → rejected.
        float err = abs(d - sceneDist) / max(min(d, sceneDist), 1.0);
        float w   = wBil[k] * exp2(-err * 16.0);

        sum  += c * w;
        wSum += w;
        if (err < nearestErr) { nearestErr = err; nearest = c; }
    }

    // All four taps mismatched (sub-texel sliver) → nearest-depth neighbour.
    if (wSum < 1.0e-4)
        return nearest;
    return sum / wSum;
}
