// CloudRaymarch.cs.hlsl
// -----------------------------------------------------------------------------
// Quarter-resolution volumetric cloud raymarch -- Nubis (Schneider 2015/2017)
// density model + Frostbite (Hillaire 2016) lighting, after UE5's
// VolumetricCloud.usf.
//
//   1. Reconstruct world-space ray from pixel UV via invViewProj.
//   2. Intersect with a SPHERICAL cloud shell around the planet (clouds dip
//      below the horizon instead of forming a flat fog band).
//   3. Clamp exit to opaque scene depth + max trace distance.
//   4. Adaptive march: coarse steps sampling the cheap base shape only;
//      on a hit, back up and switch to fine steps with detail erosion;
//      revert to coarse after several empty fine samples.
//   5. Density  = Perlin-Worley base, remap-eroded by weather coverage,
//      per-type height gradient, then high-freq Worley edge erosion
//      (wispy at the base -> billowy at the top).
//   6. Lighting = 5-tap cone + far-tap optical depth toward the sun,
//      3 Wrenninge multi-scatter octaves (a=b=c=0.5), dual-lobe HG with a
//      silver-lining lobe, Nubis in-scatter probability, height-gradient
//      ambient -- integrated with Frostbite's energy-conserving step formula.
//
// Output: RGBA16F, rgb = premultiplied in-scatter radiance, a = transmittance.
//         + R32F per-texel march distance (CloudDistOut) consumed by the
//           depth-aware composite upsample.
// -----------------------------------------------------------------------------

// Shared CB layout + RaySphere/ShellInterval/HeightInLayer.
#define CLOUD_CB_REGISTER register(b0, space2)
#include "CloudCommon.hlsli"

Texture3D<float4>         BaseNoise   : register(t0, space2);  // 128^3 R=PW GBA=worley FBM
Texture3D<float4>         DetailNoise : register(t1, space2);  // 32^3 worley erosion
Texture2D<float4>         WeatherTex  : register(t2, space2);  // 512^2 coverage/fill/type
Texture2D<float>          SceneDepth  : register(t3, space2);

SamplerState              LinearClamp : register(s0, space2);
SamplerState              LinearWrap  : register(s2, space2);  // tileable noise/weather

RWTexture2D<float4>       CloudOut    : register(u0, space2);
RWTexture2D<float>        CloudDistOut : register(u1, space2); // scene dist marched against

// ---------------------------------------------------------------------------
static const float PI             = 3.14159265;

// Sample-count ramp: full maxSteps once the in-shell segment reaches this
// length (UE: r.VolumetricCloud.DistanceToSampleMaxCount = 15 km).
static const float kDistToMaxSteps   = 15000.0;
// Horizon fade -- entry distances beyond this range blend the result away
// (the geometric horizon for a 1.5 km layer bottom is ~138 km).
static const float kHorizonFadeStart = 90000.0;
static const float kHorizonFadeEnd   = 140000.0;
// Fine->coarse hysteresis: empty fine samples before reverting to coarse.
static const int   kEmptyToCoarse    = 8;
static const float kCoarseMul        = 3.0;   // coarse step = fine step * this

// Multi-scattering octaves (Wrenninge): scatter a^n, extinction b^n, phase
// eccentricity c^n. a <= b or energy is created.
static const int   kMSOctaves = 3;
static const float kMSScatter = 0.5;   // a
static const float kMSExtinct = 0.5;   // b
static const float kMSPhase   = 0.5;   // c

// Cone kernel for the sun-light march -- decorrelated unit-ish offsets.
static const float3 kCone[5] = {
    float3( 0.30,  0.45, -0.25),
    float3(-0.35,  0.20,  0.40),
    float3( 0.45, -0.10,  0.30),
    float3(-0.20,  0.60, -0.20),
    float3( 0.15, -0.35,  0.45),
};

// ---------------------------------------------------------------------------
float Remap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) * (newHi - newLo) / max(1e-5, hi - lo);
}

float HenyeyGreenstein(float cosT, float g)
{
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosT;
    return (1.0 - g2) / (4.0 * PI * pow(max(denom, 1e-4), 1.5));
}

// Interleaved gradient noise (Jimenez) -- STATIC, deliberately not
// frame-indexed. Clouds write no velocity, so TAA partially rejects their
// history whenever the camera moves; an animated ray-start jitter then shows
// up as the whole cloudscape boiling/bobbing with camera motion. A static
// per-pixel offset converts banding into a stable spatial dither instead.
float IGN(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// ---------------------------------------------------------------------------
// Per-cloud-type density-over-height profile. gradient4 = (start0, full0,
// full1, end1); community-standard Nubis constants.
float HeightGradient(float h, float type)
{
    const float4 kStratus       = float4(0.00, 0.07, 0.08, 0.15);
    const float4 kStratocumulus = float4(0.00, 0.20, 0.42, 0.60);
    const float4 kCumulus       = float4(0.00, 0.08, 0.75, 0.98);
    float4 g = (type < 0.5)
        ? lerp(kStratus,       kStratocumulus, saturate(type * 2.0))
        : lerp(kStratocumulus, kCumulus,       saturate(type * 2.0 - 1.0));
    return smoothstep(g.x, g.y, h) * (1.0 - smoothstep(g.z, g.w, h));
}

// ---------------------------------------------------------------------------
// Cloud density at a world position. `cheap` skips the detail erosion (used
// for coarse marching and far light taps -- detail only ever REMOVES density,
// so the cheap sample is a conservative bound). `baseShape` returns the
// pre-erosion shaped density in [0,1] -- the Nubis in-scatter "lodded density"
// proxy.
float SampleDensity(float3 wp, float h, bool cheap, out float baseShape)
{
    baseShape = 0.0;

    // Weather map -- planar world XZ, scrolled at 1/4 wind speed.
    float2 wuv = (wp.xz + windOffset.xz * 0.25) * weatherScale;
    float3 weather = WeatherTex.SampleLevel(LinearWrap, wuv, 0).rgb;

    // Dual-coverage (Haggstrom): R = distinct formations; G floods toward
    // overcast as the author slider passes 0.5.
    float wCov = max(weather.r, saturate(2.0 * coverage - 1.0) * weather.g);
    float cov  = saturate(wCov * saturate(2.0 * coverage));

    // Anvil: inflate coverage toward the layer top (pow < 1 raises).
    float anvilExp = Remap(saturate(h), 0.7, 0.8, 1.0, lerp(1.0, 0.5, anvilBias));
    cov = pow(cov, clamp(anvilExp, 0.5, 1.0));
    if (cov <= 1e-4) return 0.0;

    float type = saturate(weather.b + cloudTypeBias);

    // Base shape: Perlin-Worley dilated by the Worley FBM (remap-erosion,
    // not multiplication -- keeps cores opaque).
    float3 sp = wp + windOffset;
    float4 lf = BaseNoise.SampleLevel(LinearWrap, sp * baseNoiseScale, 0);
    float lfFbm = lf.g * 0.625 + lf.b * 0.25 + lf.a * 0.125;
    float base  = saturate(Remap(lf.r, lfFbm - 1.0, 1.0, 0.0, 1.0));
    base *= HeightGradient(h, type);

    // Coverage as erosion threshold; trailing *cov softens low-coverage
    // bottoms (GP7).
    float shaped = saturate(Remap(base, 1.0 - cov, 1.0, 0.0, 1.0)) * cov;
    baseShape = shaped;
    if (cheap || shaped <= 1e-4) return shaped * density;

    // Detail erosion: wispy (inverted worley) near the base, billowy above.
    float3 hf = DetailNoise.SampleLevel(LinearWrap, sp * detailNoiseScale, 0).rgb;
    float hfFbm = hf.r * 0.625 + hf.g * 0.25 + hf.b * 0.125;
    float hfMod = lerp(hfFbm, 1.0 - hfFbm, saturate(h * 10.0));
    float eroded = saturate(Remap(shaped, hfMod * detailStrength, 1.0, 0.0, 1.0));
    return eroded * density;
}

// ---------------------------------------------------------------------------
// Optical depth toward the sun: 5 cone taps over half the layer thickness
// (full detail for the first two, cheap beyond) plus one long-range tap for
// shadows cast by distant cloud towers (HZD).
float SunOpticalDepth(float3 wp)
{
    const float layerThick = max(topAltitude - bottomAltitude, 1.0);
    const float stepLen    = (layerThick * 0.5) / 5.0;

    float od = 0.0;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        float dist = stepLen * (float(i) + 0.5);
        float3 sp  = wp + sunDir * dist + kCone[i] * (dist * 0.3);
        float  hh  = HeightInLayer(sp);
        if (hh <= 0.0 || hh >= 1.0) continue;
        float dummy;
        od += SampleDensity(sp, hh, i >= 2, dummy) * stepLen;
    }

    float3 fp = wp + sunDir * (layerThick * 1.5);
    float  fh = HeightInLayer(fp);
    if (fh > 0.0 && fh < 1.0)
    {
        float dummy;
        od += SampleDensity(fp, fh, true, dummy) * (layerThick * 0.5);
    }
    return od * extinction;
}

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint2 dt : SV_DispatchThreadID)
{
    if (dt.x >= (uint)halfResW || dt.y >= (uint)halfResH) return;

    // Reconstruct world-space ray. UV -> NDC -> world (via invViewProj).
    float2 uv  = (float2(dt) + 0.5) / float2(halfResW, halfResH);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    // Reversed-Z: near plane is NDC z=1, far is NDC z=0.
    float4 nearW = mul(float4(ndc, 1.0, 1.0), invViewProj);
    float4 farW  = mul(float4(ndc, 0.0, 1.0), invViewProj);
    float3 rayP  = nearW.xyz / nearW.w;
    float3 rayQ  = farW.xyz  / farW.w;
    float3 rayD  = normalize(rayQ - rayP);

    // FARTHEST opaque depth in this texel's 4x4 full-res footprint
    // (reversed-Z: min raw value; sky 0 is naturally the farthest). Marching
    // to the farthest surface guarantees a texel straddling a silhouette
    // holds valid cloud data for its sky pixels — the depth-aware composite
    // then picks per full-res pixel. A single centre Load made the whole
    // 4x4 block an all-or-nothing cloud decision (the terrain-outline halo).
    // Four corner gathers cover the footprint exactly.
    float sceneNdcZ;
    {
        float2 invFull = 1.0 / float2(fullResW, fullResH);
        float2 b  = float2(dt * 4u);
        float4 g0 = SceneDepth.GatherRed(LinearClamp, (b + float2(1.0, 1.0)) * invFull);
        float4 g1 = SceneDepth.GatherRed(LinearClamp, (b + float2(3.0, 1.0)) * invFull);
        float4 g2 = SceneDepth.GatherRed(LinearClamp, (b + float2(1.0, 3.0)) * invFull);
        float4 g3 = SceneDepth.GatherRed(LinearClamp, (b + float2(3.0, 3.0)) * invFull);
        float4 m  = min(min(g0, g1), min(g2, g3));
        sceneNdcZ = min(min(m.x, m.y), min(m.z, m.w));
    }

    // Scene distance this texel marches against — written for the composite's
    // depth-aware weights on EVERY exit path. Reproject (ndc, sceneNdcZ)
    // through invViewProj: convention-independent (reversed-Z safe) and
    // already a distance along this pixel's ray.
    float sceneDist = kCloudSkyDist;
    if (sceneNdcZ > 0.0)   // reversed-Z: 0 = far clear (sky), >0 = geometry
    {
        float4 surfW = mul(float4(ndc, sceneNdcZ, 1.0), invViewProj);
        sceneDist = length(surfW.xyz / surfW.w - cameraPos);
    }
    CloudDistOut[dt] = sceneDist;

    float tEntry, tExit;
    if (!ShellInterval(cameraPos, rayD, tEntry, tExit) || tEntry > kHorizonFadeEnd)
    {
        CloudOut[dt] = float4(0, 0, 0, 1);
        return;
    }

    tExit = min(tExit, sceneDist);

    // Cap the marched distance from the shell entry point (UE mode 0) and
    // remember whether the cap (not geometry/shell) ended the march so the
    // tail can be feathered instead of hard-cut.
    bool clampedByMax = (tEntry + maxTraceDist) < tExit;
    tExit = min(tExit, tEntry + maxTraceDist);
    if (tExit <= tEntry)
    {
        CloudOut[dt] = float4(0, 0, 0, 1);
        return;
    }

    const float marchLen  = tExit - tEntry;
    const float stepCount = clamp(maxSteps * saturate(marchLen / kDistToMaxSteps),
                                  32.0, maxSteps);
    const float stepFine   = marchLen / stepCount;
    const float stepCoarse = stepFine * kCoarseMul;
    const float fadeStart  = tExit - 0.25 * marchLen;   // only applied when clampedByMax

    // Per-octave phase: dual-lobe HG blended fwd/back, plus a silver-lining
    // lobe combined with max() (Nubis). Eccentricity attenuates by c^n.
    float cosT = dot(rayD, sunDir);
    float phaseOct[kMSOctaves];
    {
        float cN = 1.0;
        [unroll] for (int n = 0; n < kMSOctaves; ++n)
        {
            float ph = lerp(HenyeyGreenstein(cosT, phaseFwdG * cN),
                            HenyeyGreenstein(cosT, phaseBackG * cN),
                            phaseBlend);
            float silver = silverIntensity
                         * HenyeyGreenstein(cosT, (0.99 - silverSpread) * cN);
            phaseOct[n] = max(ph, silver);
            cN *= kMSPhase;
        }
    }

    // Ambient skylight: authored tint scaled by the sun's luminance (tracks
    // time-of-day) and a bottom-occlusion height gradient (UE: 0.5).
    const float sunLum = dot(sunColor, float3(0.299, 0.587, 0.114));
    const float3 ambientBase = ambientTint * (ambientStrength * sunLum);

    // Jittered, adaptive march. Coarse steps sample the conservative base
    // shape only; a hit backs up one coarse step and switches to fine.
    // Jitter spans ONE FINE STEP: enough to break banding, small enough that
    // the dither amplitude stays subtle in world space.
    float jitter = IGN(float2(dt));
    float t = tEntry + jitter * stepFine;

    float3 scatter       = 0;
    float  transmittance = 1.0;
    bool   fineMode      = false;
    int    sinceHit      = 0;

    const int maxIter = (int)stepCount * 3;   // coarse/fine mode switches + backups
    [loop] for (int i = 0; i < maxIter; ++i)
    {
        if (t >= tExit) break;
        float3 p = cameraPos + rayD * t;
        float  h = HeightInLayer(p);

        if (!fineMode)
        {
            float dummy;
            float dc = SampleDensity(p, h, true, dummy);
            if (dc > 1e-4)
            {
                fineMode = true;
                sinceHit = 0;
                t = max(t - stepCoarse, tEntry);   // re-cover the skipped span
                continue;
            }
            t += stepCoarse;
            continue;
        }

        float baseShape;
        float d = SampleDensity(p, h, false, baseShape);

        // Feather the tail when the max-trace cap (not geometry) ends the
        // march, so distant decks fade instead of slicing off.
        if (clampedByMax)
            d *= 1.0 - smoothstep(fadeStart, tExit, t);

        if (d > 1e-4)
        {
            sinceHit = 0;

            float od = SunOpticalDepth(p);

            // Nubis in-scatter probability -- replaces the 2015 powder term.
            // depth: how much material surrounds the sample (base shape as
            // the low-mip proxy); vertical: bases are dark, nothing scatters
            // up from below.
            float depthProb = 0.05 + pow(saturate(baseShape),
                                         clamp(Remap(h, 0.3, 0.85, 0.5, 2.0), 0.5, 2.0));
            float vertProb  = pow(clamp(Remap(h, 0.07, 0.14, 0.1, 1.0), 0.1, 1.0), 0.8);
            float inscatter = depthProb * vertProb;

            float3 ambientL = ambientBase * saturate(0.5 + h);

            // Frostbite energy-conserving integration, one term per
            // multi-scatter octave; only octave 0 advances view transmittance.
            float sigmaT = max(d * extinction, 1e-7);
            float aN = 1.0, bN = 1.0;
            float stepTr0 = 1.0;
            [unroll] for (int n = 0; n < kMSOctaves; ++n)
            {
                float sigmaS_n = sigmaT * aN;            // albedo ~ 1 (water)
                float sigmaT_n = max(sigmaT * bN, 1e-7);
                float trN = exp(-sigmaT_n * stepFine);
                float tlN = exp(-od * bN);

                float3 L = sunColor * (tlN * phaseOct[n] * inscatter);
                if (n == 0) L += ambientL;
                L *= cloudColor;

                float3 Lscat = L * sigmaS_n;
                scatter += transmittance * (Lscat - Lscat * trN) / sigmaT_n;
                if (n == 0) stepTr0 = trN;

                aN *= kMSScatter;
                bN *= kMSExtinct;
            }
            transmittance *= stepTr0;

            if (transmittance < 0.005) break;
        }
        else if (++sinceHit >= kEmptyToCoarse)
        {
            fineMode = false;
        }
        t += stepFine;
    }

    // Horizon fade -- entry distance, not sample distance, so a whole distant
    // deck fades as one.
    float horizonFade = 1.0 - smoothstep(kHorizonFadeStart, kHorizonFadeEnd, tEntry);
    scatter       *= horizonFade;
    transmittance  = lerp(1.0, transmittance, horizonFade);

    CloudOut[dt] = float4(scatter, transmittance);
}
