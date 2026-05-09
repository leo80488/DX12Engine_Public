// TAA.cs.hlsl — Temporal Anti-Aliasing resolve pass.
//
// Algorithm (Karis 2014 / Salvi 2016 hybrid):
//   1. Dilate velocity over 3x3 neighbourhood by NEAREST-DEPTH (#2).
//      Picks the foreground silhouette surface's velocity over background
//      bleed, more robust than longest-magnitude on "fast bg + slow fg".
//   2. Reproject current pixel using dilated velocity → previous-frame UV.
//   3. Build 3x3 Karis luma-weighted AABB stats + tent-filtered de-jittered
//      current sample + Karis 5-tap unsharp (#4) — one combined neighbourhood pass.
//   4. Disocclusion handling: hard out-of-bounds → spatial-AA fallback
//      (3x3 box blur of HDR neighbourhood, #11). Soft 1.5px edge inset (#1)
//      ramps history confidence so Catmull-Rom edge clamp does not ghost.
//   5. Sample history at reprojected UV (9-tap Catmull-Rom). Salvi 5-tap
//      cross blur on history luma (#7) for anti-flicker comparison.
//   6. Variance-clip history; gamma widens for specular / low-variance / high-
//      freq-static neighbourhoods. In tonemap-blend mode gamma is rescaled
//      ×1.5 to compensate for tonemap-compressed sigma (#3).
//   7. Karis tonemap → blend → inverse tonemap; velocity-adaptive alpha plus
//      bright-peak / outline / high-freq fixes. lumaGain anti-flicker uses
//      bidirectional |Δluma| (#6); velFactor band tightened to (0.5, 2.5) (#8);
//      adaptive firefly ratio scales by neighbourhood mean luma (#9).
//   8. Output stores prev luma in .a (#10) for downstream AutoExposure use.
//
// Modules:
//   TAA_Common.hlsli        — TAACB + colour-space / Karis tonemap / clip helpers
//   TAA_Reproject.hlsli     — depth-aware DilateVelocity, ReprojectUV
//   TAA_History.hlsli       — SampleHistoryCatmullRom9Tap
//   TAA_Neighbourhood.hlsli — ComputeNeighbourhood (3x3 stats + de-jitter + sharpen taps)
//
// Root signature (compute space2, matches GraphicsDX12::CreateComputeRootSignature):
//   b0 space2 — TAACB
//   t0 space2 — current jittered HDR scene
//   t1 space2 — depth (R32_FLOAT view of D32_FLOAT)
//   t2 space2 — history (previous resolved frame)
//   t3 space2 — GBuffer surface (roughness in .r, metallic in .g)
//   t4 space2 — velocity (NDC units, R16G16_FLOAT)
//   u0 space2 — output UAV (current resolved frame, becomes next history)

#include "TAA_Common.hlsli"
#include "TAA_Reproject.hlsli"
#include "TAA_History.hlsli"
#include "TAA_Neighbourhood.hlsli"

Texture2D<float4>   gCurrHDR  : register(t0, space2);
Texture2D<float>    gDepth    : register(t1, space2);
Texture2D<float4>   gHistory  : register(t2, space2);
Texture2D<float4>   gGBuffer  : register(t3, space2);
Texture2D<float2>   gVelocity : register(t4, space2);
RWTexture2D<float4> gOutput   : register(u0, space2);

SamplerState        gLinear   : register(s0, space2);

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    int2   dim = int2(width, height);
    float2 res = float2(dim);
    float2 uv  = (float2(id) + 0.5) / res;

    // ---- 3x3 neighbourhood (always; needed for spatial-AA fallback too) -----
    // Hoisted above the disocclusion early-out so the unweighted 3x3 mean is
    // available as a soft spatial-AA fallback when history is unusable
    // (#11: replaces the old "raw aliased curr" passthrough).
    NeighbourhoodStats nh = ComputeNeighbourhood(gCurrHDR, int2(id), dim,
                                                  jitterX, jitterY);
    float3 currRaw = nh.centerHDR;          // raw jittered centre tap (Fix G path)
    float3 currBox = nh.meanRGB_HDR_unweighted;  // 3x3 box-blurred HDR (spatial-AA fallback)

    // ---- No history (first frame or resize) -> spatial-AA fallback ----------
    // Previously passthrough raw aliased curr; now use the box-blurred 3x3 mean
    // so the first frame after a teleport / scene cut isn't visibly jagged.
    if (hasHistory < 0.5)
    {
        gOutput[id] = float4(currBox, Luma(currBox));
        return;
    }

    // ---- Specular detection -------------------------------------------------
    // isSpecular: 0 = rough/diffuse, 1 = mirror-like specular.
    // Used to widen the AABB and increase history weight, reducing specular jitter.
    float roughness  = gGBuffer.SampleLevel(gLinear, uv, 0).r;
    float isSpecular = 1.0 - smoothstep(0.0, specularRoughnessMax, roughness);

    // ---- Reprojection via depth-aware velocity dilation ---------------------
    // Karis 2014: pick the velocity from the 3x3 neighbour with smallest depth
    // (nearest to camera under reversed-Z, that means the LARGEST .r value).
    // Better than longest-magnitude on "fast bg + slow fg" silhouettes — the
    // foreground edge always wins, so we never reproject foreground pixels
    // along background motion.
    float2 velocity = DilateVelocity(gVelocity, gDepth, int2(id), dim);

    float2 prevUV = ReprojectUV(uv, velocity);

    // ---- Disocclusion: hard out-of-bounds → spatial-AA fallback (#11) -------
    // Catmull-Rom 9-tap reads ±1.5 px around prevUV; if prevUV itself is
    // outside [0,1] history can't recover and we fall back to the spatial mean.
    if (IsUVOutside(prevUV))
    {
        gOutput[id] = float4(currBox, Luma(currBox));
        return;
    }

    // ---- Edge confidence (#1): 1.5 px inset soft fade -----------------------
    // Even when prevUV is "in range", Catmull-Rom 9-tap will still sample 1.5
    // px outside the texture at its outer lobes. The sampler's CLAMP behaviour
    // duplicates edge texels and produces spurious history, which manifests
    // as a thin ghost band along screen borders during camera motion. Ramp
    // history confidence to 0 within 1.5 px of any edge: alpha gets nudged
    // toward 1 (full new sample) instead of pulling stale clamped history.
    float2 edgePx     = min(prevUV, 1.0 - prevUV) * res;
    float  edgeConf   = saturate(min(edgePx.x, edgePx.y) - 0.5);  // 0 inside 0.5px, 1 by 1.5px

    float3 m1    = nh.m1;
    float3 sigma = nh.sigma;
    float3 curr  = nh.currFilt;   // tent-de-jittered current

    // ---- Noise-adaptive AABB gamma ------------------------------------------
    // isSpecular (from roughness) captures smooth/mirror specular well, but
    // rough specular (roughness 0.3-0.5) has isSpecular ≈ 0.1-0.35 even though
    // its per-pixel noise is just as severe.  Use the neighbourhood Y-channel
    // variance as a secondary indicator: high sigma.x → noisy specular region.
    float isNoisySpecular = smoothstep(0.02, 0.2, sigma.x);
    float isSpecularFull  = saturate(isSpecular + isNoisySpecular * (1.0 - isSpecular));

    // (#3) Tonemap-blend mode rescale: σ is collected in tonemapped YCoCg
    // space which compresses the dynamic range of highlights and therefore
    // shrinks σ relative to HDR-linear σ. The CB-side tunables (default
    // 1.5 / 2.0) are calibrated for HDR-linear semantics so the editor
    // sliders stay intuitive across modes; bake the ×1.5 compensation here.
#if TAA_USE_TONEMAP_BLEND
    const float kTonemapSigmaScale = 1.5;
#else
    const float kTonemapSigmaScale = 1.0;
#endif
    float effectiveGamma  = lerp(colorBoxSigma, colorBoxSigmaSpecular, isSpecularFull) * kTonemapSigmaScale;

    // Fix C: widen AABB for very-low-variance neighbourhoods (sky, distant geometry).
    // Narrow gamma on those pixels produces a tiny box that rejects valid history on
    // sub-pixel jitter, which manifests as distant shimmer. Relax the clip there.
    float isLowVariance = 1.0 - smoothstep(0.01, 0.05, sigma.x);
    effectiveGamma      = lerp(effectiveGamma, effectiveGamma * 1.5, isLowVariance);

    // Velocity → pixel magnitude. Hoisted up from the blend block below because
    // the high-frequency-static widener (Fix E) below needs it before the clip.
    //
    // (#8) smoothstep band tightened from (1.0, 4.0) to (0.5, 2.5). The old
    // 1 px dead-zone failed to detect sub-pixel camera micro-motion on
    // hi-DPI displays — stationary-but-actually-drifting pixels got no
    // velocity-boosted alpha and showed faint ghost trails. (0.5, 2.5)
    // matches modern AAA practice (UE5 reference is (0.0, 1.5), Frostbite
    // ~(0.5, 2.0)); the upper bound 2.5 prevents 8-px overshoot from
    // saturating alpha and creating ghost trails on slow-moving objects.
    float  velPx     = length(velocity * res * 0.5);
    float  velFactor = smoothstep(0.5, 2.5, velPx);

    // Fix E v2: distant thin sub-pixel geometry anti-aliasing (fences, wires,
    // foliage) — bimodal classifier.
    //
    // The original v1 keyed off `smoothstep(0.03, 0.12, sigma.x)`, which fired
    // on TWO different signals that look identical in σ alone:
    //   (a) sub-pixel fence: 3×3 has roughly half slat / half background;
    //       centre tap is one of the two modes (high-contrast bimodal dist).
    //   (b) smooth contrast gradient (e.g. a chair-leg shadow boundary):
    //       3×3 is a linear ramp; centre tap sits midway through the ramp
    //       i.e. close to the mean (unimodal distribution).
    // (a) needs the wide-AABB / raw-centre-tap path; (b) absolutely does not
    // — relaxing its clip introduces visible ghosting along shadow edges
    // (the user's "chair-leg false-positive").
    //
    // The discriminator: how far the centre tap sits from the neighbourhood
    // mean, normalised by σ. For a Gaussian distribution only ~13% of pixels
    // exceed 1.5σ; fence/edge pixels are bimodal (≈ half-and-half), so by
    // construction the centre tap lands at roughly ±2σ from the mean of the
    // half-and-half distribution. Gradient pixels are unimodal and the centre
    // rarely exceeds ~0.7σ.
    //
    // Smoothstep window (1.2σ → 2.0σ) keeps the transition gentle; below 1.2σ
    // we definitely have a gradient, above 2.0σ we definitely have a bimodal
    // pattern, in between is fence-with-noise.
    //
    // sigma threshold tightened from (0.03, 0.12) to (0.05, 0.15): now that
    // bimodal does the main discrimination, σ only needs to gate out very-
    // low-variance noise — the loose floor was over-firing.
    float bimodalDist = abs(nh.centerY - nh.m1.x) / max(nh.sigma.x, 1e-3);
    float bimodal     = smoothstep(1.2, 2.0, bimodalDist);

    float isHighFreqStatic = smoothstep(0.05, 0.15, sigma.x)
                           * bimodal
                           * (1.0 - velFactor);
    effectiveGamma         = lerp(effectiveGamma, 4.0, isHighFreqStatic);

    // Fix L: Karis-dimming protection. During camera motion the 3x3 neighbourhood
    // rarely captures the brightest sub-pixel features (jittered peak lands on
    // neighbouring pixels each frame), and the Karis luma weight 1/(1+L) further
    // deweights the few peaks that DO survive. The resulting AABB is too tight
    // for bright history values reprojected via velocity — they get clipped down
    // to yMax every frame and bright tree highlights / leaf sparkle visibly
    // DARKEN during motion, only recovering when the camera stops (jitter
    // eventually re-includes the peaks in the centre tap and the AABB widens).
    //
    // Mitigation: widen the AABB proportional to motion magnitude. Static
    // pixels keep tight clamping (no ghosting); fast-moving pixels get up to
    // (1 + velocityWiden) × the box width. Cost is slightly higher motion
    // ghost tolerance — humans tolerate ghost much better than luminance
    // pulsing during motion, so the trade is favourable.
    //
    // Gated by (1 - isHighFreqStatic) because Fix E above ALREADY pushed
    // gamma to 4.0 on fence pixels via lerp; multiplying by (1+vel*widen) on
    // top of that compounds and produces a >5σ box that defeats the variance
    // clip entirely. By construction Fix E's path also sets velFactor≈0 (the
    // path only fires on near-static pixels), so this gate is a defence in
    // depth — under transient motion onto/off-of fence pixels, both could
    // be partially active simultaneously and the compounding shows up.
    effectiveGamma *= 1.0 + velFactor * velocityWiden * (1.0 - isHighFreqStatic);

    // (#4) Karis 5-tap unsharp mask on the de-jittered curr.
    //
    // Tent de-jitter is a wide low-pass filter (its 1-px lobes touch all 8
    // neighbours) — over many accumulated frames the integrated history
    // gradually trends softer than the underlying signal. A 5-tap unsharp
    //   sharp = curr*5 - (top + bottom + left + right)
    // restores a single pixel's worth of high-frequency contrast without
    // adding ringing on smooth gradients (the cardinals all sit equidistant,
    // so a uniform region reduces to curr*5 - curr*4 = curr → no shift).
    //
    // Apply BEFORE the Fix G high-freq-static lerp: sharpening then replacing
    // with raw center on fence pixels means the sharpen never affects fence
    // reconstruction.
    //
    // Bright-peak gate (regression fix 2026-05-08): for a center pixel that is
    // already 3-5x brighter than its cardinal mean (a specular peak / leaf
    // sparkle / sun glint), the unsharp formula 5×curr - cardinals further
    // amplifies the peak by ~5x. That bigger value enters AABB clip, lerps
    // into history, and accumulates over frames into bloom-triggering
    // brightness. peakRatio = Luma(curr) / Luma(cardinalMean) ramps the
    // sharpen off across (2x, 5x) so normal contrast keeps full sharpen
    // and bright outliers don't get amplified.
    //
    // Specular gate (shimmer fix 2026-05-08): the unsharp formula amplifies
    // the frame-to-frame difference between centre and cardinals by 5x.
    // On a specular surface that frame-to-frame difference IS the jitter-
    // induced BRDF peak shift — sharpening it amplifies the very flicker
    // we want TAA to integrate out. The 0.1 alpha cap can't compress the
    // 5x-amplified swing back to invisibility; observation: sharpenStrength
    // 0 = no shimmer, > 0 = shimmer scales with strength. Specular surfaces
    // get their high-freq detail from physics (BRDF lobes), not from
    // texture/geometry, so they don't BENEFIT from sharpen either —
    // disabling it loses no perceived sharpness, only kills the artefact.
    if (sharpenStrength > 0.0)
    {
        float3 sharp           = curr * 5.0 - nh.cardinalSum;
        float3 cardinalMean    = nh.cardinalSum * 0.25;
        float  peakRatio       = Luma(curr) / max(Luma(cardinalMean), 1e-3);
        float  brightPeakKill  = smoothstep(2.0, 5.0, peakRatio);
        float  sharpKill       = saturate(isHighFreqStatic + brightPeakKill + isSpecularFull);
        float  effStrength     = sharpenStrength * (1.0 - sharpKill);
        curr = lerp(curr, sharp, effStrength);
    }

    // Fix G: on high-freq-static pixels go FULLY to the raw jittered centre
    // tap. Tent de-jitter is correct for smooth regions but catastrophic for
    // 1-pixel-wide high-contrast geometry: it averages the fence pixel with
    // 8 background neighbours, collapsing the jitter-phase coverage signal
    // that TAA relies on for sub-pixel reconstruction. Each frame jitter
    // places the sub-pixel geometry at a slightly different screen position;
    // the RAW sample encodes "this frame, this pixel is X% slat" for that
    // specific jitter phase. TAA then integrates X across frames into the
    // correct coverage value.
    //
    // currRaw was already extracted in ComputeNeighbourhood — no extra Load.
    curr = lerp(curr, currRaw, isHighFreqStatic);

    // HDR-space neighbourhood mean from the combined 3x3 pass — used by
    // Fix K below and the bright-peak detection further down.
    //
    // Use the UNWEIGHTED 1/9·Σs mean (not the Karis-weighted m1 inv-tonemapped
    // back). The Karis weight 1/(1+L) was designed to deweight outliers when
    // building the AABB; using that same biased mean as the "ratio threshold"
    // in Fix K below double-punishes legitimate sub-pixel HDR spikes — the
    // weighted mean is dragged down by the dimmer 8 of 9 samples, the spike
    // sits at "currLuma > 8·tinyMean" → gets clamped down to ~tinyMean·8,
    // which is still well below its real value, so leaf sparkle ends up
    // crushed regardless of how the AABB then treats it. The unweighted mean
    // is the actual spatial average; "8× of that" is a much fairer ceiling.
    float3 meanRGB_HDR = nh.meanRGB_HDR_unweighted;
    float  meanLumaHDR = max(Luma(meanRGB_HDR), 1e-3);

    // Fix K: HDR soft clamp (UE4-style input pre-expansion). For foliage /
    // leaf sparkle the underlying signal is a sub-pixel HDR spike — one
    // pixel's shaded luma can be 50x its neighbourhood because of a sunlit
    // leaf edge that only catches light through jitter. That spike enters
    // the clip with its huge luma, gets clipped in TAA's AABB (which is
    // built from Karis-weighted stats that barely see the peak), history
    // never integrates it → flicker survives every anti-flicker rule we
    // try. Clamping curr to 8x neighbourhood mean BEFORE clip means fewer
    // extreme peaks enter history, and downstream logic (clip, alpha)
    // operates on sane values. Tuned: 8x allows legitimate specular
    // highlights (car paint, metal, wet surfaces) to remain bright while
    // cutting off leaf-scale fireflies; raise to 16x if legitimate bright
    // highlights look dimmed, drop to 4x for aggressive firefly removal.
    //
    // (#9) Ratio is now adaptive on neighbourhood mean luma. A fixed 8x is
    // simultaneously too loose for dim indoor (where firefly-noise is barely
    // above ambient and 8x lets too much through) and too tight for bright
    // outdoor HDR (where legitimate sun glints / specular peaks legitimately
    // hit 10x+ neighbourhood). Ramp from 4x at very dim to 16x at bright
    // sunlight-scale, with smoothstep so the transition isn't abrupt across
    // a bright/dim seam (like a window edge in a dark room).
    {
        float currLumaHDR = Luma(curr);
        float kFireflyRatio = lerp(4.0, 16.0, smoothstep(0.1, 5.0, meanLumaHDR));
        float maxAllowed = meanLumaHDR * kFireflyRatio;
        if (currLumaHDR > maxAllowed)
            curr *= maxAllowed / currLumaHDR;
    }

    float3 nMin = m1 - effectiveGamma * sigma;
    float3 nMax = m1 + effectiveGamma * sigma;

    // ---- Sample history with 9-tap Catmull-Rom + variance-clip --------------
    // Clamp to >= 0: Catmull-Rom negative lobes can produce sub-zero values that
    // flip sign after ToneMapLuma (denominator < 1) and amplify into fireflies.
    float3 histLinear = max(SampleHistoryCatmullRom9Tap(gHistory, gLinear,
                                                         prevUV * res, res),
                            0.0);
    float  histPrevLuma = gHistory.SampleLevel(gLinear, prevUV, 0).a;  // (#10) plumbed luma channel

    // (#7) Salvi 5-tap cross blur on history luma — supplies the lumaGain
    // anti-flicker comparison reference. Per Salvi 2016 the simple lumaH = Luma(hist)
    // misses high-freq specular flicker (1-px sparkle on leaves / water /
    // wet metal) because hist itself contains the same frequency the flicker
    // sits at; comparing against a SPATIALLY-SMOOTHED history luma decouples
    // detection from the flickering signal. Taps are in HDR linear so we
    // can do the comparison on a consistent space regardless of TAA_USE_TONEMAP_BLEND.
    float lumaHistSmooth;
    {
        float2 uvStep = 1.0 / res;
        float3 hN = gHistory.SampleLevel(gLinear, prevUV + float2(0.0,         -uvStep.y), 0).rgb;
        float3 hS = gHistory.SampleLevel(gLinear, prevUV + float2(0.0,          uvStep.y), 0).rgb;
        float3 hE = gHistory.SampleLevel(gLinear, prevUV + float2( uvStep.x,    0.0     ), 0).rgb;
        float3 hW = gHistory.SampleLevel(gLinear, prevUV + float2(-uvStep.x,    0.0     ), 0).rgb;
        lumaHistSmooth = 0.2 * (Luma(histLinear) + Luma(hN) + Luma(hS) + Luma(hE) + Luma(hW));
    }

#if TAA_USE_TONEMAP_BLEND
    float3 hist = ToneMapLuma(histLinear);
#else
    float3 hist = histLinear;
#endif
    float3 histYCoCg = RGBToYCoCg(hist);
    float  histY     = histYCoCg.x;   // pre-clip Y, saved for distance-to-clamp below
    hist = YCoCgToRGB(ClipAABB(nMin, nMax, histYCoCg));

    // ---- Frame-rate-independent velocity-adaptive blend ---------------------
    // isSpecularFull drives both the tau and the lumaGain so that rough noisy
    // specular gets the same long-history / tracking treatment as smooth speculars.
    //
    // Fix F REMOVED. It was biasing `currT` toward the 3×3 tonemapped mean
    // on high-freq-static pixels, which is structurally wrong for 1-pixel-
    // wide high-contrast geometry: the spatial mean of a 3×3 window around
    // one fence slat is 1/9 slat + 8/9 background ≈ background, so pulling
    // the blend target toward it makes the fence FADE instead of integrate.
    // Correct model: each frame the pixel's coverage of the sub-pixel
    // geometry is some value X (from the current jitter phase); TAA must
    // preserve that per-frame X and integrate it across frames — NOT push
    // it toward a spatial average that is structurally biased.
#if TAA_USE_TONEMAP_BLEND
    float3 currT = ToneMapLuma(curr);
#else
    float3 currT = curr;   // linear-blend mode: legacy name, no tonemap
#endif
    // ============================================================================
    // (#12) FINAL ALPHA COMPOSITION ORDER
    //
    // alpha = lerp(diffuse, specular, isSpecFull)               (1) baseAlpha — frame-rate-independent τ
    //       → lerp(α, max(α, 0.3), velFactor)                   (2) velocity boost — moving pixels track
    //       + lumaGain · isSpec · (maxFromLuma - α)             (3) anti-flicker (specular only) — bidirectional |Δluma|
    //       × distToClamp / (distToClamp + boxW)                (4) Salvi/Karis distance-to-clamp (multiplicative ↓)
    //       max α, isOutlineEdge · 0.9                          (5) outline rejection — UPPER override
    //       lerp(α, min(α, 0.1), brightPeakDamp)                (6) bright-peak damp (lowers α)
    //       lerp(α, min(α, 0.03), highFreqStatic · !outline)    (7) fence/wire damp (lowers α)
    //       lerp(α, min(α, 0.03), mirrorDamp · !outline)        (7b) mirror-shimmer damp (lowers α; roughness<0.1 + low velocity)
    //       lerp(1.0, α, edgeConf)                              (8) edge-fade override — α→1 within 1.5px of border
    //
    // Final α range is clamped by the strongest active rule:
    //   • outline (5) sets a high floor — once active α ≥ 0.9 regardless of upstream damps
    //   • highFreqStatic (7) sets a low ceiling — once active α ≤ 0.03 unless outline already raised it
    //   • edge-fade (8) is the LAST step and overrides everything when within 1.5 px of screen border
    //     (history confidence is structurally low there — Catmull-Rom is sampling clamped edge texels)
    // ============================================================================
    float  safeTau       = max(tauHistory, 1e-4);
    float  diffuseAlpha  = 1.0 - exp(-deltaTime / safeTau);
    float  specularAlpha = 1.0 - exp(-deltaTime / (safeTau * 8.0));
    float  baseAlpha     = lerp(diffuseAlpha, specularAlpha, isSpecularFull);
    // velPx / velFactor computed earlier (hoisted above the clip block for
    // Fix E). (#8) smoothstep(0.5, 2.5, velPx) — see the velFactor comment
    // above for the rationale on the tightened band vs the old (1, 4).
    float  alpha         = lerp(baseAlpha, max(baseAlpha, 0.3), velFactor);

    // ---- Luminance-gain anti-flicker (specular / noisy regions only) --------
    //
    // (#6) Bidirectional |Δluma|: previous saturate((lumaC - lumaH) / sum)
    // only fired when curr was BRIGHTER than hist — bright→dark transitions
    // (a moving specular highlight leaving the pixel) didn't trigger
    // anti-flicker, so the dim-side of the blink looked fine but the
    // bright-side over-shot. abs() makes both directions trigger the
    // alpha cap toward maxAlphaFromLuma.
    //
    // (#7) lumaH compares against the Salvi-smoothed history luma (HDR
    // linear, computed above) rather than Luma(hist). lumaC is taken in HDR
    // linear too (Luma(curr)) so both sides live in the same space — the
    // ratio is space-invariant anyway, but mixing tonemapped and HDR-linear
    // would skew the threshold for bright pixels.
    //
    // Mirror-shimmer fix (2026-05-08): mix in the TEMPORAL-smoothed luma
    // stored in history.a by (#10) — Salvi's spatial blur catches sub-pixel
    // peak shifts between adjacent pixels, but on a chrome surface the peak
    // can also flicker on/off in time at the same pixel. The temporal
    // signal is the slow-blended Luma of the previous resolved output;
    // mixing 30% of it into the reference makes anti-flicker robust in
    // both spatial and temporal dimensions. 0.3 weight chosen so a single-
    // frame outlier in the temporal channel cannot dominate (it'd over-
    // ride the spatial signal at higher weights and re-introduce flicker).
    float lumaH    = lerp(lumaHistSmooth, histPrevLuma, 0.3);
    float lumaC    = Luma(curr);
    float lumaGain = saturate(abs(lumaC - lumaH) / max(lumaC + lumaH + 0.01, 0.01));
    float maxAlphaFromLuma = lerp(0.05, 0.1, isSpecularFull);
    alpha = saturate(alpha + lumaGain * isSpecularFull * (maxAlphaFromLuma - alpha));

    // ---- Falcor distance-to-clamp anti-flicker ------------------------------
    // Karis 2014 / Salvi 2016: when pre-clip history Y sits near (or beyond)
    // the AABB Y boundary, the variance clamp is doing meaningful work and
    // bringing in the full new sample would visibly flicker. Scale alpha by
    //   d / (d + boxWidth)
    // so blending leans toward history when history is being squeezed by the
    // clamp, and stays near baseline when history is comfortably inside the
    // box. Pure scalar luminance — chrominance flicker is rarely perceptible.
    //
    // Composition note: this multiplicatively reduces alpha. The downstream
    // outline rejection (max alpha) and bright-peak / high-freq-static damps
    // (lerp toward min) still take precedence — that ordering is intentional
    // so outlines stay sharp and bright fireflies still get crushed.
    if (antiFlicker != 0)
    {
        float yMin = m1.x - effectiveGamma * sigma.x;
        float yMax = m1.x + effectiveGamma * sigma.x;
        float distToClamp = min(abs(yMin - histY), abs(yMax - histY));
        float boxW = max(yMax - yMin, 1e-5);
        alpha = saturate(alpha * distToClamp / (distToClamp + boxW));
    }

    // ---- Bright-peak detection (for outline rejection gate + anti-flicker) --
    // Fix I: detection is now in HDR space, not tonemapped. ToneMapLuma
    // compresses a 50x HDR peak down to ~2x ratio in tonemapped space,
    // which sat right at the edge of the old smoothstep(1.5, 3.0) and
    // gave isBrightPeak ≈ 0.15 — barely any damping on real fireflies.
    // HDR ratios for leaf/sun peaks are easily 5-20x; the (2.0, 8.0) range
    // captures the realistic dynamic range without false-firing on normal
    // contrast. `meanLumaHDR` hoisted above (used by Fix K too).
    // Note: after Fix K, `curr` is already firefly-clamped, so currLumaHDR
    // is bounded — isBrightPeak still fires for cases Fix K let through
    // (e.g. peak was at 6x, under K's 8x threshold, but still wants damp).
    float currLumaHDR  = Luma(curr);
    float isBrightPeak = smoothstep(2.0, 8.0, currLumaHDR / meanLumaHDR);

    // ---- Outline ghost rejection -------------------------------------------
    // Outline pixels have high color contrast but near-zero velocity (they're
    // post-process, not geometry). When the color difference between current
    // and clipped history is large AND velocity is small, force higher alpha
    // to reject stale history (prevents outline ghosting/trails).
    //
    // Gated by (1 - isBrightPeak) so sub-pixel highlights never trigger the
    // rejection: outline edges have mid-range luma transitions, bright peaks
    // are luminance spikes above the local background — distinguishable.
    float3 colorDiff = abs(currT - hist);
    float  diffMag   = max(colorDiff.r, max(colorDiff.g, colorDiff.b));
    // Fix B (rebalanced): the raised diffMag threshold (0.15→0.35 band) is
    // already self-gating against ordinary texture/specular contrast, so the
    // velocity gate is dropped entirely. The earlier `step(velPx, 0.3)` meant
    // outline-rejection went to zero the moment a character started moving,
    // so the new character-position outline got no alpha boost — old outline
    // survived in history → visible ghost / "outline drops out" on motion.
    //
    // Outlines are post-process: they don't appear in the velocity buffer, so
    // they need history REJECTION on their own merit (colour delta alone).
    // isBrightPeak guard still prevents sub-pixel highlights from triggering.
    float  isOutlineEdge = smoothstep(0.15, 0.35, diffMag)
                         * (1.0 - isBrightPeak);
    // Outline α target raised from 0.4 → 0.9. At 0.4 the blend was
    // 0.6*history + 0.4*current, which dilutes the outline (history at the
    // moving character's new silhouette is the wrong colour — background,
    // not outline). 0.9 leaves 10% history for stability on the wider
    // isOutlineEdge ramp without visibly softening the line. If you see
    // flicker on character boundaries, back off to ~0.7.
    alpha = max(alpha, isOutlineEdge * 0.9);

    // ---- Bright-peak anti-flicker ------------------------------------------
    // For static bright peaks, force alpha down toward a small value so the
    // per-frame jittered luminance averages out through the history. Only
    // applies when there is truly no motion — moving bright features should
    // still track via their velocity vector.
    //
    // Fix J: use a LOOSER static gate than velFactor for this specific damp.
    // Leaves in wind, water surfaces, and grass all have small non-zero
    // velocity (0.5-2 px from sway), but their specular spikes still need
    // damping — the standard velFactor range (1, 4) would already be
    // shutting flickerDamp off at 1 px. `nearStatic` keeps damping active
    // through mild sub-pixel motion where bright-peak aliasing is most
    // visible.
    float nearStatic  = 1.0 - smoothstep(0.5, 3.0, velPx);
    float flickerDamp = isBrightPeak * nearStatic;
    alpha = lerp(alpha, min(alpha, 0.1), flickerDamp);

    // Fix H: alpha crush for high-freq-static pixels. At baseAlpha≈0.19
    // (60fps, τ=0.08s) every frame pulls the result 19% toward "this
    // frame's jitter phase" — even if history has perfectly integrated
    // the right coverage, that 19% swing visibly disturbs the integration.
    // Crushing to 0.03 means only 3% new info per frame → ~33-frame window
    // for history to converge and any single-frame phase error is diluted
    // 30x. Gated against outline-edge because outlines legitimately have
    // high σ + zero velocity (they're post-process), but need FAST
    // responsiveness (the `max(alpha, 0.9)` above) not slow integration.
    alpha = lerp(alpha, min(alpha, 0.03), isHighFreqStatic * (1.0 - isOutlineEdge));

    // ---- Mirror-shimmer α crush (chrome / wet / very-low roughness) ---------
    // Very-low-roughness specular surfaces (roughness < 0.1: polished metal,
    // wet ground, mirror) produce per-frame highlight wobble because the
    // sub-pixel specular peak shifts between adjacent pixels with each
    // jitter phase. The standard specular path's α≈0.04 (60fps, τ×8) plus
    // lumaGain pushing α back toward 0.1 gives a 10% per-frame swing —
    // visible as continuous shimmer.
    //
    // Crush α to 0.03 (33-frame window) when isMirror AND near-static.
    // Trade-off: slow response to legitimate mirror content change, but the
    // mirror's reflected scene is itself TAA-resolved so the only thing
    // sampling at full rate would be camera motion — and camera motion
    // gives velPx > 0.5 which gates this off (nearStatic falls to 0).
    //
    // Gated against isOutlineEdge so post-process outlines on mirror
    // surfaces still respond fast.
    float isMirror     = 1.0 - smoothstep(0.0, 0.2, roughness);
    float mirrorDamp   = isMirror * (1.0 - smoothstep(0.5, 3.0, velPx)) * (1.0 - isOutlineEdge);
    alpha = lerp(alpha, min(alpha, 0.02), mirrorDamp);

    // (#1) Edge-fade: pixels within 1.5 px of any screen border get
    // edgeConf < 1, fading α toward 1.0 (full new sample, no history).
    // edgeConf was computed above based on prevUV * res. Last alpha modifier
    // so it overrides every previous rule when at the very border — even
    // outline rejection, which would otherwise insist on stale clamped history.
    alpha = lerp(1.0, alpha, edgeConf);

    float3 resolved = lerp(hist, currT, alpha);

    // ---- Output -------------------------------------------------------------
    // (#10) Alpha channel carries an exponentially-blended prev luma for
    // downstream AutoExposure. Slow blend (0.05 toward current, 0.95 keep
    // history) under steady state; on disocclusion (alpha→1) we let
    // currLuma in immediately so exposure doesn't lag through teleports.
    // The blend coefficient mixes alpha (already a disocclusion proxy)
    // with a slow steady-state floor — when alpha is small (good history),
    // luma evolves slowly; when alpha is large (motion / disocclusion),
    // luma tracks the current frame.
    float resolvedLumaHDR;
#if TAA_USE_TONEMAP_BLEND
    // Saturation guard (regression fix 2026-05-08): max-channel ToneMapLuma
    // saturates at max(channel)=1.0; values arbitrarily close to 1 inverse-
    // amplify by 1/(1-max). After variance-clip + lerp on a wide AABB
    // (especially with #3's σ ×1.5 rescale and specular gamma stacking),
    // tonemapped channels can land at 0.98-0.999, producing 50-1000x HDR
    // spikes that accumulate into history every frame and trigger bloom.
    // Cap proportionally at 0.99 → max amplification 99x, well above
    // Fix K's adaptive 16x mean cap so legitimate HDR specular highlights
    // are unaffected — only the saturation runaway is bounded.
    {
        float maxR = max(max(resolved.r, resolved.g), resolved.b);
        if (maxR > 0.99) resolved *= 0.99 / maxR;
    }
    float3 outRGB = max(InvToneMapLuma(resolved), 0.0);
#else
    float3 outRGB = max(resolved, 0.0);
#endif
    resolvedLumaHDR = Luma(outRGB);
    float lumaBlend  = max(alpha, 0.05);
    float outLuma    = lerp(histPrevLuma, resolvedLumaHDR, lumaBlend);
    gOutput[id] = float4(outRGB, outLuma);
}
