// TAA.cs.hlsl — Temporal Anti-Aliasing resolve pass.
//
// Algorithm (Karis 2014 / Salvi 2016 hybrid):
//   1. Dilate velocity over 3x3 neighbourhood by NEAREST-DEPTH (#2).
//      Picks the foreground silhouette surface's velocity over background
//      bleed, more robust than longest-magnitude on "fast bg + slow fg".
//   2. Reproject current pixel using dilated velocity → previous-frame UV.
//   3. Build 3x3 Karis luma-weighted AABB stats + Blackman-Harris (Karis 2014)
//      de-jittered current sample + Karis 5-tap unsharp (#4) — one combined
//      neighbourhood pass. Reconstruction switched from tent → BH on 2026-05-20
//      to address static softness (tent's wide separable footprint low-passed
//      every frame; BH concentrates energy at the centre tap).
//   4. Disocclusion handling: hard out-of-bounds → spatial-AA fallback
//      (3x3 box blur of HDR neighbourhood, #11). Soft 1.5px edge inset (#1)
//      ramps history confidence so Catmull-Rom edge clamp does not ghost.
//   5. Sample history at reprojected UV (9-tap Catmull-Rom). Salvi 5-tap
//      cross blur on history luma (#7) for anti-flicker comparison.
//   6. Variance-clip history; gamma widens for specular / low-variance / high-
//      freq-static neighbourhoods. In tonemap-blend mode gamma is rescaled
//      ×1.5 to compensate for tonemap-compressed sigma (#3).
//   7. Karis tonemap → blend → inverse tonemap; velocity-adaptive alpha plus
//      bright-peak / high-freq fixes. lumaGain anti-flicker uses
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
//   t6 space2 — prev-frame depth (TAA-owned ping-pong, R32_FLOAT)
//   t7 space2 — prev-frame velocity (TAA-owned ping-pong, R16G16_FLOAT)
//   u0 space2 — output UAV (current resolved frame, becomes next history)
//   u2 space2 — prev-depth out UAV (writes curr depth → next-frame prev)
//   u3 space2 — prev-velocity out UAV (writes curr velocity → next-frame prev)
// (slots t6/t7/u2/u3 are reused root params — defined in the global compute
//  root sig for VolumetricFog and XeGTAO; harmlessly free for TAA's own use.)

#include "TAA_Common.hlsli"
#include "TAA_Reproject.hlsli"
#include "TAA_History.hlsli"
#include "TAA_Neighbourhood.hlsli"

Texture2D<float4>   gCurrHDR       : register(t0, space2);
Texture2D<float>    gDepth         : register(t1, space2);
Texture2D<float4>   gHistory       : register(t2, space2);
Texture2D<float4>   gGBuffer       : register(t3, space2);
Texture2D<float2>   gVelocity      : register(t4, space2);
// Stencil-plane view of the depth buffer (X24_TYPELESS_G8_UINT). .y holds the
// stencil byte; we test against outlineStencilBit (set by OutlinePass on the
// rim pixels written by the inverted-hull sub-pass). 0 if the SRV isn't bound
// — guarded by outlineStencilBit==0 from the CB so the load is skipped.
Texture2D<uint2>    gOutlineStencil: register(t5, space2);
Texture2D<float>    gPrevDepth     : register(t6, space2);
Texture2D<float2>   gPrevVelocity  : register(t7, space2);
RWTexture2D<float4> gOutput        : register(u0, space2);
RWTexture2D<float>  gPrevDepthOut  : register(u2, space2);
RWTexture2D<float2> gPrevVelOut    : register(u3, space2);

SamplerState        gLinear        : register(s0, space2);

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

    // Compute velocity early so we can write prev-depth/prev-velocity snapshots
    // to UAVs BEFORE any early-return path. Otherwise the first frame after
    // resize / teleport leaves prev buffers uninitialised → next frame's
    // disocclusion detector reads garbage and false-fires.
    float  currDepthVal = gDepth.Load(int3(id, 0)).r;
    float2 velocity     = DilateVelocity(gVelocity, gDepth, int2(id), dim);
    gPrevDepthOut[id] = currDepthVal;
    gPrevVelOut[id]   = velocity;

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
    // velocity computed above the hasHistory early-return so prev-vel UAV stays valid.
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
    float3 curr  = nh.currFilt;   // Blackman-Harris de-jittered current (Karis 2014)

    // Hoisted velPx/velFactor — needed by isNoisySpecular's motion gate below.
    // Tightened smoothstep band (0.5, 2.5) — see comment block further down for
    // the historical reasoning (UE5/Frostbite parity, sub-pixel micro-motion).
    float  velPx     = length(velocity * res * 0.5);
    float  velFactor = smoothstep(0.5, 2.5, velPx);

    // ---- Noise-adaptive AABB gamma ------------------------------------------
    // isSpecular (from roughness) captures smooth/mirror specular well, but
    // rough specular (roughness 0.3-0.5) has isSpecular ≈ 0.1-0.35 even though
    // its per-pixel noise is just as severe.  Use the neighbourhood Y-channel
    // variance as a secondary indicator: high sigma.x → noisy specular region.
    //
    // Motion gate (1 - velFactor): during motion, sigma is artificially
    // elevated by jitter+motion-induced sub-pixel sample variation, NOT by
    // genuine noisy-specular. Without this gate, motion makes isNoisySpecular
    // fire → isSpecularFull spikes → sharpKill kills the Karis sharpen →
    // visible motion blur. The gate keeps sharpen alive during motion so
    // moving content stays sharp.
    float isNoisySpecular = smoothstep(0.02, 0.2, sigma.x) * (1.0 - velFactor);
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

    // velPx / velFactor hoisted earlier (above the noise-adaptive gamma block).
    // Smoothstep band (0.5, 2.5) matches UE5 (0.0, 1.5) / Frostbite ~(0.5, 2.0);
    // upper bound 2.5 prevents 8-px overshoot from saturating alpha and
    // creating ghost trails on slow-moving objects.

    // High-freq-static detector for fences / wires / foliage / lamp boundaries.
    //
    // Symmetric bimodal: centre tap close to either extreme of unweighted
    // yMin/yMax (Karis-weighted m1 is biased dim, so previous |center-m1|/σ
    // only fired for bright-centre patterns and missed dark-centre).
    //   binary {A,B} 50/50 → bimodalRef=0, bimodal=1 (both centres) ✓
    //   linear gradient {-1,0,+1} (center=0) → bimodalRef≈σ, bimodal=0 ✓
    float bimodalRef  = min(abs(nh.centerY - nh.yMin),
                             abs(nh.centerY - nh.yMax));
    float bimodalDist = bimodalRef / max(nh.sigma.x, 1e-3);
    float bimodal     = 1.0 - smoothstep(0.4, 1.0, bimodalDist);

    // Contrast gate in HDR-LINEAR space, NOT tonemapped σ. Tonemap c/(1+max)
    // collapses HDR-bright bimodal (lamp HDR 5 vs 100 → tonemapped 0.83 vs 0.99
    // → σ ≈ 0.08) below the σ-gate's mid-band. (max-min)/mid in HDR linear
    // stays large for HDR-bright sources and matches the LDR σ gate for
    // LDR-only fences.
    float hdrRange     = nh.lumaMaxHDR - nh.lumaMinHDR;
    float hdrMid       = max(0.5 * (nh.lumaMaxHDR + nh.lumaMinHDR), 1e-3);
    float hdrContrast  = hdrRange / hdrMid;
    float contrastGate = smoothstep(0.2, 0.6, hdrContrast);

    // HDR-bright bypass for the bimodal centre check. For HDR sources (lamps,
    // fireworks, glints) sub-pixel jitter often lands the centre tap on a
    // partial-coverage aliased value mid-way between the two modes — bimodal
    // centre check misses it. HDR contrast alone is signal enough; smooth
    // LDR gradients don't reach HDR brightness so no false-positive risk.
    float isHDRBright = smoothstep(2.0, 8.0, nh.lumaMaxHDR);
    float bimodalEffective = lerp(bimodal, 1.0, isHDRBright);

    // Motion gate uses (1 - velFactor) — smoothstep(0.5, 2.5, velPx). Restored
    // from a previously-tightened (0.3, 1.0) experiment: jitter alone produces
    // a sub-pixel velocity component (~0.4-0.7 px/frame from per-frame jitter
    // delta), and the tight gate let it partially-shut the damp in static
    // scenes → spec edge flicker returned. The (0.5, 2.5) gate ignores jitter
    // entirely. Slow-motion disocclusion no longer relies on this gate to
    // release the damp — disoccBoost (depth/vel/clip detector below) does
    // that job by force-raising α.
    float isHighFreqStatic = contrastGate
                           * bimodalEffective
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
    // clip entirely.
    //
    // Density-aware widen / shrink for the AABB during motion.
    //
    // - Dense LDR neighbourhoods (tile / foliage / brick) under motion: CR
    //   sub-pixel sampling pulls in colour from the foreground's previous
    //   pixel; that contaminated hist easily fits inside any motion-widened
    //   box and survives variance clip → motion ghost. Counter by:
    //     a. Killing widen entirely for LDR dense (1 - density when LDR).
    //     b. Actively SHRINKING effectiveGamma by up to 50% for LDR dense
    //        motion → tighter clip catches the sub-pixel contamination.
    // - HDR-bright neighbourhoods (lamps / stars / glints): keep the full
    //   widen so jitter+motion-displaced bright peaks stay inside AABB and
    //   reprojected bright history isn't crushed.
    float neighbourhoodDensity = smoothstep(0.05, 0.15, sigma.x);
    float ldrDenseMotion       = neighbourhoodDensity * velFactor * (1.0 - isHDRBright);
    float effectiveWiden       = velocityWiden * (1.0 - neighbourhoodDensity * (1.0 - isHDRBright));
    effectiveGamma *= 1.0 + velFactor * effectiveWiden * (1.0 - isHighFreqStatic);
    effectiveGamma *= 1.0 - ldrDenseMotion * 0.5;

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
        // Motion gate added 2026-05-17, BAND LOOSENED 2026-05-20.
        //
        // Original logic: `curr*5 - cardinals` amplifies centre-vs-cardinal
        // delta 5×; under camera motion the centre tap shifts every frame
        // because jitter + motion lands it on different sub-pixel content,
        // and amplifying that swing 5× translates into shimmer. Gating
        // sharpen by velFactor=(0.5,2.5) was meant to disable sharpen
        // "during motion" and keep static surfaces full-sharp.
        //
        // Problem: the Halton(2,3) jitter sequence produces a per-frame
        // jitter delta of ~0.5–0.7 px on its own, so velPx is RARELY 0 in
        // practice — even a perfectly still camera has velPx ≈ 0.5,
        // landing right at the start of smoothstep(0.5, 2.5). Tiny camera
        // microtwitches push it to 1+ which fully shuts sharpen down.
        // Result: "static observation" was effectively always-unsharp,
        // contributing to the perceived TAA softness.
        //
        // New band smoothstep(2.0, 8.0, velPx) only fires on motion that
        // genuinely accumulates >2 px of true world motion per frame
        // (running speed / camera flick). Static + jitter + slow walk all
        // keep full sharpen. The 5×-amplification shimmer concern is now
        // handled by Blackman-Harris reconstruction (TAA_Neighbourhood.hlsli)
        // which doesn't pre-spread the centre tap across the 3x3 — so the
        // delta sharpen amplifies is the genuine signal, not jitter noise.
        float  sharpVelKill    = smoothstep(2.0, 8.0, velPx);
        float  sharpKill       = saturate(isHighFreqStatic + brightPeakKill + isSpecularFull + sharpVelKill);
        float  effStrength     = sharpenStrength * (1.0 - sharpKill);
        curr = lerp(curr, sharp, effStrength);
    }

    // For LDR fence-style sub-pixel geometry, replace the reconstructed curr
    // with the raw centre tap — even BH still folds in some cardinal/corner
    // weight (~0.1 + ~0.01), which would collapse the jitter-phase coverage
    // signal TAA needs for sub-pixel reconstruction of 1-px-wide features.
    // HDR-bright cases SKIP this (currRaw oscillates between bright/dark per
    // jitter phase for HDR sources; the BH-reconstructed centre is more
    // temporally stable for multi-pixel-wide content).
    curr = lerp(curr, currRaw, isHighFreqStatic * (1.0 - isHDRBright));

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

    // Motion-time AABB Y expansion — HDR-bright neighbourhoods ONLY. Karis-
    // weighted m1/σ undercount bright pixels so HDR stars/spec peaks fall
    // outside the box during motion; expanding to unweighted yMin/yMax saves
    // them. For LDR neighbourhoods (no bright outliers to preserve), this
    // expansion just widens the box and lets CR-sampled contamination
    // through, producing visible motion ghost on dense-σ backgrounds (tile
    // floors etc.) — gated by isHDRBright so LDR keeps the tight Karis box.
    float aabbExpand = velFactor * isHDRBright;
    if (aabbExpand > 0.0)
    {
        nMin.x = lerp(nMin.x, min(nMin.x, nh.yMin), aabbExpand);
        nMax.x = lerp(nMax.x, max(nMax.x, nh.yMax), aabbExpand);
    }

    // ---- Sample history with 9-tap Catmull-Rom + variance-clip --------------
    // Clamp to >= 0: Catmull-Rom negative lobes can produce sub-zero values that
    // flip sign after ToneMapLuma (denominator < 1) and amplify into fireflies.
    //
    // Motion-time bilinear blend: CR's 9-tap spreads over a 4×4 pixel area, so
    // at sub-pixel prevUV offsets it picks up colour from pixels that USED to
    // contain a moving foreground feature (thin column / wire / pole). With
    // dense-σ neighbourhoods (tile floor, foliage) the contaminated hist stays
    // inside the wide AABB and survives variance clip → visible ghost trail
    // behind the moving feature. Bilinear's 2×2 tap area stays closer to
    // prevUV and doesn't reach the contaminated neighbour. Smoothly cross-fade
    // CR → bilinear with motion: static keeps CR's sharpness, fast motion
    // gets bilinear's reduced contamination (motion blur perception masks
    // bilinear's slight softness).
    float3 histLinearCR    = SampleHistoryCatmullRom9Tap(gHistory, gLinear,
                                                          prevUV * res, res);
    float3 histLinearBilin = gHistory.SampleLevel(gLinear, prevUV, 0).rgb;
    // Band tightened (0.5, 2.0) → (8.0, 20.0) on 2026-05-17 — normal walking /
    // panning motion peaks well under 8 px/frame, and at (0.5, 2.0) CR fully
    // collapsed to bilinear, whose 2×2 tap footprint visibly soft-focuses
    // textures during motion. CR's negative-lobe `max(0,…)` clamp already
    // handles its own contamination; only genuine fast swings (camera flick)
    // need the bilinear fallback now.
    float  useBilinear     = smoothstep(8.0, 20.0, velPx);
    float3 histLinear      = max(lerp(histLinearCR, histLinearBilin, useBilinear), 0.0);
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

    // Fast-swing HDR clip bypass: when camera swings fast, the current 3x3 may
    // miss sub-pixel bright features entirely. If history Y exceeds the
    // unweighted neighbourhood max AND we're moving fast enough that velocity
    // reprojection trust outweighs disocclusion risk AND the neighbourhood is
    // genuinely HDR-bright (isHDRBright gate prevents LDR disocclusion ghost
    // — e.g. a moving character revealing a wall would also satisfy the
    // histY > yMax test, but isHDRBright=0 keeps the bypass off there).
    float3 histClipped = YCoCgToRGB(ClipAABB(nMin, nMax, histYCoCg));
    float3 histRaw     = YCoCgToRGB(histYCoCg);
    float  bypassClip  = smoothstep(1.0, 5.0, velPx)
                       * smoothstep(0.0, 0.2, histY - nh.yMax)
                       * isHDRBright;
    hist = lerp(histClipped, histRaw, bypassClip);

    // Disocclusion signal via clip magnitude. The variance clip's job is to
    // flag "this history is wrong" — if it had to move hist substantially,
    // this pixel had a content change (foreground passed by, occluder
    // revealed). Used downstream as an UPWARD α-boost (disoccBoost) rather
    // than as a damp gate: pushing α to 0.6 makes curr dominate the blend
    // and the wrong hist decays in 1-2 frames; just gating the damp would
    // leave α at 0.3 baseline and let the polluted hist enter at 70%.
    float3 clipDelta = histRaw - histClipped;
    float  clipMag   = max(max(abs(clipDelta.r), abs(clipDelta.g)), abs(clipDelta.b));

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
    // FINAL ALPHA COMPOSITION ORDER
    //
    // alpha = lerp(diffuse, specular, isSpecFull)               (1) baseAlpha — frame-rate-independent τ
    //       → lerp(α, max(α, 0.3), velFactor)                   (2) velocity boost — moving pixels track
    //       + lumaGain · isSpec · (maxFromLuma - α)             (3) anti-flicker (specular only) — bidirectional |Δluma|
    //       × distToClamp / (distToClamp + boxW)                (4) Salvi/Karis distance-to-clamp (multiplicative ↓)
    //       lerp(α, min(α, 0.1), brightPeakDamp)                (5) bright-peak damp (lowers α)
    //       lerp(α, min(α, 0.03), highFreqStatic)               (6) fence/wire damp (lowers α)
    //       lerp(α, min(α, 0.01), distantSpecDamp)              (6b) distant specular extra crush (α=0.01)
    //       lerp(α, min(α, 0.02), mirrorDamp)                   (7) mirror-shimmer damp (roughness<0.1 + low velocity)
    //       lerp(1.0, α, edgeConf)                              (8) edge-fade override — α→1 within 1.5px of border
    //
    // Final α range is clamped by the strongest active rule:
    //   • highFreqStatic (6) sets a low ceiling — once active α ≤ 0.03
    //   • distantSpecDamp (6b) tightens further to ≤ 0.01 on specular surfaces
    //   • edge-fade (8) is the LAST step and overrides everything when within 1.5 px of screen border
    //     (history confidence is structurally low there — Catmull-Rom is sampling clamped edge texels)
    // ============================================================================
    // Direct history weight (2026-05-24, replaces the τ-seconds model — see
    // TAA_Common.hlsli for rationale). The 0.125 spec multiplier preserves
    // the old τ*8 ratio so specular tracking remains slow (longer history)
    // relative to diffuse without forcing a second slider on the user.
    float  diffuseAlpha  = 1.0 - saturate(historyWeight);
    float  specularAlpha = diffuseAlpha * 0.125;
    float  baseAlpha     = lerp(diffuseAlpha, specularAlpha, isSpecularFull);
    // velPx / velFactor computed earlier (hoisted above the clip block for
    // Fix E). (#8) smoothstep(0.5, 2.5, velPx) — see the velFactor comment
    // above for the rationale on the tightened band vs the old (1, 4).
    // Velocity boost target lowered 0.5 → 0.15 (2026-05-17). 0.5 = 50% new
    // sample per moving frame was effectively a 2-frame box filter under
    // sustained camera motion — HDR sub-pixel highlights aliased differently
    // each jitter phase and the high α let the per-frame alias straight into
    // history → shimmer. Real disocclusion path below still uses 0.5; this
    // baseline boost only widens history weight for ordinary motion.
    float  alpha         = lerp(baseAlpha, max(baseAlpha, 0.15), velFactor);

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
    // bright-peak / high-freq-static damps (lerp toward min) still take
    // precedence so bright fireflies still get crushed.
    if (antiFlicker != 0)
    {
        float yMin = m1.x - effectiveGamma * sigma.x;
        float yMax = m1.x + effectiveGamma * sigma.x;
        float distToClamp = min(abs(yMin - histY), abs(yMax - histY));
        float boxW = max(yMax - yMin, 1e-5);
        alpha = saturate(alpha * distToClamp / (distToClamp + boxW));
    }

    // ---- Bright-peak detection (for anti-flicker damping) ------------------
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

    // Fix H: alpha crush for high-freq-static pixels. Crushing to 0.03 means
    // only 3% new info per frame → ~33-frame window for history to converge.
    alpha = lerp(alpha, min(alpha, 0.03), isHighFreqStatic);

    // Distant specular extra crush. high-freq-static + specular = sub-pixel
    // spec peak shifting between adjacent pixels with peak/dim 20-50x. α=0.03
    // still leaves visible per-frame swing; α=0.01 (≈100-frame window) drops
    // it below visibility.
    float distantSpecDamp = isHighFreqStatic * isSpecularFull;
    alpha = lerp(alpha, min(alpha, 0.01), distantSpecDamp);

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
    float isMirror     = 1.0 - smoothstep(0.0, 0.2, roughness);
    float mirrorDamp   = isMirror * (1.0 - smoothstep(0.5, 3.0, velPx));
    alpha = lerp(alpha, min(alpha, 0.02), mirrorDamp);

    // Disocclusion α-boost via depth + clipMag (max), gated by (1 - isHDRBright).
    //
    //   depthMM via 1-tap point sample of prev-depth at prevUV. Catches the
    //     dense-σ-bg-with-thin-foreground case where AABB is so wide that
    //     CR contamination fits inside (clipMag stays 0). A column at a
    //     different depth than the bg trivially trips depthMM regardless
    //     of how wide the colour AABB is.
    //   clipMag-based detector for coplanar disocclusion (foreground and
    //     background at similar depth) where depth signal fails.
    //
    // Both HDR-gated. HDR-bright pixels rely on spec damps (fix-H/H+,
    // mirror, bright-peak); the depth-mismatch from sub-pixel jitter at
    // HDR spec edges would otherwise α-boost and bypass them, causing
    // edge flicker that's hard to fix without making the detector too
    // insensitive for LDR cases.
    //
    // Boost target 0.5 = 3-frame ≈ 50ms convergence. Modest enough that
    // residual false-fires aren't visible flashes.
    {
        int2  prevPxI    = clamp(int2(prevUV * res), int2(0, 0), dim - 1);
        float prevDepthV = gPrevDepth.Load(int3(prevPxI, 0)).r;
        float depthMM    = abs(currDepthVal - prevDepthV) / max(currDepthVal, 1e-5);

        float ldrGate     = 1.0 - isHDRBright;
        float disoccDepth = smoothstep(0.04, 0.12, depthMM) * ldrGate;
        float disoccClip  = smoothstep(0.05, 0.2,  clipMag) * ldrGate;
        float disocclusion = max(disoccDepth, disoccClip);

        alpha = lerp(alpha, max(alpha, 0.5), disocclusion);
    }

    // ---- Outline-aware history weakening -----------------------------------
    // OutlinePass stamps a stencil bit on every rim pixel painted by the
    // inverted-hull sub-pass. Rim pixels share the BACKGROUND's velocity and
    // depth (the hull writes colour but not depth), so default TAA reprojects
    // them as if they were stationary background — leaving a several-frame
    // outline trail behind a moving silhouette. Floor α to outlineMinAlpha
    // (default 0.5 ≈ 3-frame convergence) on these pixels so the new outline
    // colour wins the blend within ~50 ms instead of taking 25+ frames.
    //
    // Gated by outlineStencilBit being non-zero, which the C++ side clears
    // when no stencil SRV was plumbed in (e.g. depth format isn't D24_S8).
    // Placed AFTER every damp so it can't be undone by Fix-H/H+/mirror/etc.,
    // but BEFORE edge-fade (#1) — screen-border pixels need α→1 regardless of
    // their outline state because Catmull-Rom is sampling clamped texels.
    if (outlineStencilBit != 0)
    {
        uint stencilByte = gOutlineStencil.Load(int3(id, 0)).y;
        if ((stencilByte & outlineStencilBit) != 0)
            alpha = max(alpha, outlineMinAlpha);
    }

    // (#1) Edge-fade: pixels within 1.5 px of any screen border get
    // edgeConf < 1, fading α toward 1.0 (full new sample, no history).
    // edgeConf was computed above based on prevUV * res. Last alpha modifier
    // so it overrides every previous rule when at the very border —
    // history confidence is structurally low there because Catmull-Rom is
    // sampling clamped edge texels.
    alpha = lerp(1.0, alpha, edgeConf);

    float3 resolved = lerp(hist, currT, alpha);

    // Fast-swing HDR max-protect with decay. Prevents sub-pixel HDR features
    // (stars, distant spec peaks) from vanishing during rapid camera swings
    // (CR sub-pixel hist sampling + occasional miss-frames cumulatively dim
    // bright sources). max(resolved, hist * decay) preserves hist against
    // dim-curr washout while allowing bounded fade so camera moving PAST a
    // bright source doesn't leave permanent ghost.
    //
    // Decay 0.88 + per-frame CR (≈0.7) + dim-curr blend gives ≈0.6 per-frame
    // decay → bright source fades to invisible in ~6-10 frames (~150ms at
    // 60fps). Trade-off chosen to balance star preservation vs ghost trail.
    //
    // Tightened gate (8, 20) so only genuinely fast swings engage the
    // max-protect — moderate camera motion relies on the AABB Y expansion
    // and clip bypass above (which don't introduce ghost).
    float fastMotionHDR = isHDRBright * smoothstep(8.0, 20.0, velPx);
    resolved = lerp(resolved, max(resolved, hist * 0.88), fastMotionHDR);

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
    // (gPrevDepthOut / gPrevVelOut already written near top of shader so the
    //  no-history / out-of-bounds early-return paths leave valid data.)
}
