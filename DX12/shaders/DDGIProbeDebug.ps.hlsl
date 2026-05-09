// DDGIProbeDebug.ps.hlsl — colour each probe sphere by its octahedral
// irradiance atlas sample at the surface normal. The colour you see at any
// point on a probe sphere is the irradiance that a surface oriented in that
// direction would receive from indirect bounce — making it visually obvious
// which probes have data and what direction the indirect comes from.

#include "DDGICommon.hlsli"
#include "DDGISampling.hlsli"

ConstantBuffer<DDGIVolumeGPU>  g_Vol           : register(b1, space0);
StructuredBuffer<DDGIProbeSH>  g_ProbeSH       : register(t0, space0);
SamplerState                   g_LinearSampler : register(s0, space0);

cbuffer DebugConstants : register(b2, space0)
{
    float g_SphereRadius;
    uint  g_DebugMode;     // 0 = SH irradiance (default)
                           // 1 = grid-coord color (verify probe placement)
                           // 2 = SH L0 magnitude as heat (verify SH has data)
                           // 3 = SH directionality (verify SH evolves frame-to-frame)
    uint  _padR1, _padR2;
};

struct PSIn
{
    float4 pos      : SV_POSITION;
    float3 normal   : NORMAL;
    uint   probeIdx : PROBE_IDX;
};

float4 main(PSIn i) : SV_TARGET
{
    // Recover probe grid coord so the debug-mode 1 grid colour matches the
    // probe's index even though the SH eval below uses the linear probeIdx.
    int3 coord = DDGI_ProbeCoord(i.probeIdx, g_Vol);

    // Diagnostic mode 1: bypass atlas sampling, output a pre-tone-mapped
    // colour derived from probe coord (R=+x, G=+y, B=+z). Lets us tell
    // whether bloom / artefacts come from atlas data or from the rendering
    // pipeline itself. If probes show sane RGB grid colours here but go
    // crazy in mode 0, the atlas is the culprit.
    uint   probeIdx = (uint)i.probeIdx;
    DDGIProbeSH sh  = g_ProbeSH[probeIdx];

    if (g_DebugMode == 1)
    {
        float3 c = float3(
            float(coord.x) / max(float(g_Vol.probeCountsX - 1), 1.0),
            float(coord.y) / max(float(g_Vol.probeCountsY - 1), 1.0),
            float(coord.z) / max(float(g_Vol.probeCountsZ - 1), 1.0));
        // Half-strength so they don't bloom even without tone-mapping.
        return float4(c * 0.5, 1.0);
    }

    if (g_DebugMode == 2)
    {
        // L0 magnitude per channel — directly visualises the "ambient" SH
        // term written by relight. If this is zero everywhere, relight isn't
        // writing. If it varies probe-to-probe, SH is being populated.
        float3 l0 = float3(sh.R.w, sh.G.w, sh.B.w);
        float  m  = length(l0);
        // Tone-map so values >> 1 still display well.
        return float4(l0 / (1.0 + m), 1.0);
    }

    if (g_DebugMode == 3)
    {
        // Per-channel L1 magnitude — i.e., how much "directional info" is in
        // each colour channel of the SH. Tone-mapped so even small values are
        // visible.
        //
        // Diagnostic semantics:
        //   * All-black → L1 coefficients are zero (relight is averaging out
        //     direction info — likely all rays returning same colour).
        //   * Stable colours that DON'T change when you rotate the sun → SH
        //     is being read but not rewritten (relight CS isn't dispatching
        //     or its writes aren't reaching the SRV used by Lighting.ps).
        //   * Colours shift when sun moves → relight + Lighting.ps are wired
        //     correctly; any "fixed" feel is just convergence-rate / noise.
        float3 l1mag = float3(length(sh.R.xyz),
                              length(sh.G.xyz),
                              length(sh.B.xyz));
        float  m     = length(l1mag);
        return float4(l1mag / (1.0 + m), 1.0);
    }

    float3 n = normalize(i.normal);
    float3 irr = DDGI_SH_Irradiance(sh, n);

    // Tone-map the probe colour into roughly LDR range. The atlas stores
    // physical irradiance which can easily be 5-10 in HDR (HDR sky + sun),
    // and writing those values straight into the HDR scene texture makes
    // bloom blow them up into giant fluorescent orbs that hide the actual
    // colour we want to see. Reinhard `x / (1 + x)` keeps everything below
    // 1.0 while preserving relative colour. The +0.02 ambient floor makes
    // a brand-new (zero atlas) probe still visible as a faint dark sphere.
    irr += 0.02;
    float3 toneMapped = irr / (1.0 + irr);
    return float4(toneMapped, 1.0);
}
