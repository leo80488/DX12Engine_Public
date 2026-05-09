// Skybox.ps.hlsl — pixel shader for the skybox cube.
// Samples the environment cubemap + draws an analytic sun disk on top.
// The sun disk is tinted orange / red at sunrise / sunset via a simple
// wavelength-dependent Beer-Lambert approximation of atmospheric transmittance.

TextureCube gEnvironment : register(t6, space0);
// Moon disk texture (square 2D). Active only when SkyCB.IsMoon == 1 — we still
// need a valid bind even when the sun is up, so SkyboxPass binds whatever it
// has. The shader gates sampling on the flag so an arbitrary 2D texture in the
// slot is harmless during the day.
Texture2D<float4> gMoonTex   : register(t17, space0);
// Pre-baked starfield cubemap (R11G11B10F). Generated once at startup by
// StarsBake.cs.hlsl; runtime PS just samples + scales by the day/night blend.
TextureCube gStarsTex        : register(t18, space0);
SamplerState gSampler        : register(s0);

// Sky / Sun / Moon constants.
// SunDir points TOWARDS the *active body* (sun OR moon) in world space (unit).
// When IsMoon != 0 the body is the moon and we sample gMoonTex onto its disk.
cbuffer SkyCB : register(b2, space0)
{
    float3 SunDir;         float SunDiskSize;     // cos(half-angle of the sharp core)
    float3 SunColor;       float SunDiskIntensity;
    float3 MoonDir;        float MoonDiskCos;
    float3 MoonColor;      float MoonVisible;     // 1 = draw moon disk, 0 = skip
    // Starfield params — pushed by Renderer per-frame.
    float  StarIntensity;  // [0,1] day/night blend (0 = invisible, 1 = full night)
    float  StarTime;       // seconds, drives twinkle phase
    float  StarDensity;    // grid resolution (~250 → ~few thousand visible stars)
    float  StarBrightness; // overall multiplier
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float3 dir : TEXCOORD0;
};

// ---------------------------------------------------------------------------
// Starfield — pre-baked into a TextureCube by StarsBake.cs.hlsl.
//   .rgb = star colour × size × magnitude (pre-multiplied)
//   .a   = per-star twinkle phase ∈ (0, 1], or 0 where no star exists
//
// The alpha-packed phase is the key to non-flickering twinkle: every texel
// that belongs to the same star has the SAME phase (hash is cell-scoped).
// After bilinear sampling the phase stays roughly constant across the star's
// pixel footprint, so all pixels of one star modulate together → reads as
// "one object twinkling", not per-pixel noise.
// ---------------------------------------------------------------------------
float3 RenderStars(float3 rd)
{
    if (StarIntensity <= 0.0) return float3(0, 0, 0);

    float horizonFade = smoothstep(-0.05, 0.20, rd.y);
    if (horizonFade <= 0.0) return float3(0, 0, 0);

    float4 baked = gStarsTex.SampleLevel(gSampler, rd, 0);
    if (baked.a <= 0.0) return float3(0, 0, 0);  // empty sky

    // Per-star twinkle. Phase from baked.a (constant across all texels of
    // the same star → pixels of one star modulate in lockstep). Wide swing
    // [0.20, 1.40] = 7× brightness ratio so the effect is obvious even after
    // tonemapping compresses the upper end. Speed 3 rad/s ≈ 0.5 Hz full
    // cycle, slow enough to not feel nervous.
    const float kTwinkleSpeed = 3.0;
    float phase  = baked.a * 6.28318530718;
    float tw     = 0.90 + 0.30 * sin(StarTime * kTwinkleSpeed + phase);

    // Day/night power curve.
    float fade = StarIntensity * StarIntensity;

    return baked.rgb * tw * horizonFade * fade * StarBrightness;
}

// Approximate atmospheric transmittance to the sun at elevation angle `elev`
// (sin(elev) = SunDir.y). Higher path length near the horizon filters blue +
// green more than red, giving the classic orange-red sunset tint.
// Coefficients tuned so zenith ≈ (1, 1, 1) and horizon ≈ (0.85, 0.42, 0.12).
float3 SunAtmosphericTint(float sinElev)
{
    // 1 / max(sin(elev), 0.02) — clamped path length for sub-horizon angles.
    float pathLen = 1.0 / max(sinElev + 0.03, 0.03);
    // Wavelength-dependent Rayleigh optical depth (λ^-4). These numbers give
    // a visually pleasing sunset without needing a LUT lookup.
    float3 od = float3(0.10, 0.36, 0.90) * pathLen;
    return exp(-od);
}

float4 main(PSIn i) : SV_TARGET
{
    float3 rd  = normalize(i.dir);
    float3 sunDirN = normalize(SunDir);
    float3 sky = gEnvironment.SampleLevel(gSampler, rd, 0.0f).rgb;

    // Stars — additive on top of the sky cubemap, gated by StarIntensity
    // (Renderer-driven day/night blend). Drawn before the moon disk so the
    // moon overwrites stars that fall under its silhouette.
    //
    // Per-pixel twinkle is NOT applied here: `dot(rd, float3(12.9,78.2,45.1))
    // * 43758` gave every pixel an essentially random phase, so the pixels
    // making up one star would modulate independently — the star never
    // looked like "one object twinkling", it looked like a shimmering patch
    // of noise. A correct twinkle needs a *per-star* phase (constant across
    // the pixels of the same star), which requires either baking the phase
    // into the cubemap alpha or re-running the nearest-star hash per pixel
    // (defeating the whole point of the pre-bake). Left out for now; the
    // baked cubemap gives a clean, stable starfield.
    sky += RenderStars(rd);

    // Analytic sun disk — sharp, pixel-resolution, no cubemap aliasing.
    //
    // SunDiskSize = cos(core half-angle). The real solar disk is ≈ 0.27°
    // (cos ≈ 0.99999) but we widen slightly to ~0.4° so a 1-pixel-wide disk
    // doesn't disappear at low resolutions. The glow halo fades out over
    // the next ~1° past the core so the sunset looks natural.
    float cosT = dot(rd, sunDirN);
    float core = smoothstep(SunDiskSize - 2e-4, SunDiskSize, cosT);

    // Glow: cosine-space width is roughly 1 − cos(1°) ≈ 0.00015. Scale it
    // proportional to the disk size so tightening SunDiskSize also tightens
    // the halo.
    float glowStart = SunDiskSize - 0.0005; // ~1.8° of glow around the disk
    float glow      = pow(saturate((cosT - glowStart) / 0.0005), 4.0);

    // Horizon fade — hides the disk smoothly below the horizon so you don't
    // see it beneath the ground plane.
    float horizonMask = smoothstep(-0.02, 0.02, sunDirN.y);

    // Atmospheric tint on the disk itself (horizon sun is orange/red).
    float3 tint = SunAtmosphericTint(sunDirN.y);

    // Sun disk — the analytic path is always evaluated (its own horizonMask
    // gates it against the ground plane) so daytime picks up the crisp sun
    // disk exactly as before.
    float  coreAmt = (core + glow * 0.25) * SunDiskIntensity * horizonMask;
    float3 color   = sky + SunColor * tint * coreAmt;

    // Moon disk — rendered independently of which body drives lighting. The
    // moon's horizon mask extends ~4.5° below 0 so as the moon sets it fades
    // out smoothly in the night sky instead of popping off at exactly 0°
    // elevation. Conversely it also fades in as the moon crests horizon.
    if (MoonVisible > 0.0)
    {
        float3 moonDirN = normalize(MoonDir);
        float  moonCosT = dot(rd, moonDirN);

        // Fade the disk while it's still a few degrees above horizon so the
        // moon dims out gradually instead of clinging brightly until it
        // clips. Fully visible above +0.12 (~7°), fully gone by +0.01.
        float moonHorizon = smoothstep(0.01, 0.12, moonDirN.y);

        if (moonHorizon > 0.0 && moonCosT > 0.0)
        {
            float3 upHint    = (abs(moonDirN.y) > 0.99) ? float3(0, 0, 1) : float3(0, 1, 0);
            float3 tangent   = normalize(cross(upHint, moonDirN));
            float3 bitangent = cross(moonDirN, tangent);

            float sinHalf = sqrt(saturate(1.0 - MoonDiskCos * MoonDiskCos));
            float u = dot(rd, tangent)   / max(sinHalf, 1e-4);
            float v = dot(rd, bitangent) / max(sinHalf, 1e-4);

            float r2     = u * u + v * v;
            float inside = saturate(1.0 - r2);
            float mask   = smoothstep(0.0, 0.01, inside) * moonHorizon;

            if (mask > 0.0)
            {
                float2 uv  = float2(u, v) * 0.5 + 0.5;
                float4 tex = gMoonTex.SampleLevel(gSampler, uv, 0.0);
                color += tex.rgb * tex.a *  mask;
            }
        }
    }

    return float4(color, 1.0f);
}

