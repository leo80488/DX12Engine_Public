// XeGTAOTemporal.cs.hlsl — AO temporal accumulation over DENOISED input.
//
// Pipeline order (C++ side, XeGTAOPass::Execute):
//   Main(raw 1-spp blue-noise AO) → Spatial Denoise(edge-aware 3×3 bilateral)
//     → this pass → LightingPass
//
// The earlier Main → Temporal → Denoise ordering was structurally broken: the
// variance clip ran on raw AO whose σ was so wide it accepted essentially
// every stale history value as "inside the clip window", which is why crevice
// and enclosed-corner ghosts persisted for 10+ frames no matter what α or
// rejection threshold we set. Running the spatial denoise *first* collapses σ
// to a tight, real neighbourhood spread, turning the variance clip into a
// proper disocclusion detector. With that in place α can sit at ~0.95 without
// fighting noise, because the input is already clean.
//
// Compute root signature (space2):
//   b0 — TemporalCB
//   t0 — denoised AO this frame      (R8_UNORM)   ← spatial-denoise output
//   t1 — packed XeGTAO edge mask     (R8_UNORM)
//   t2 — prev-frame accumulated AO   (R8_UNORM)   ← AO history (ping-pong)
//   t3 — prev-frame linear depth     (R32_FLOAT)  ← depth history (ping-pong)
//   t4 — NDC velocity buffer         (R16G16_FLOAT)
//   t5 — current linear depth (mip 0 of the XeGTAO pyramid, R32_FLOAT)
//   u0 — this-frame accumulated AO   (R8_UNORM)   ← write (FINAL output)
//   u1 — this-frame linear depth     (R32_FLOAT)  ← write (next frame's t3)
//
// Reprojection: current-frame UV minus GBuffer NDC velocity → previous UV.
// History rejection layered:
//   1. Screen-edge fade (off-screen reproj)
//   2. Clamp-delta disocclusion (variance clip catches AO-space mismatch)
//   3. Edge-mask amplified rejection (geometric edges are disocclusion-prone)
//   4. PLANE-DISTANCE rejection — compare current linear Z vs reprojected
//      prev linear Z. The classic dynamic-object reveal case (a hand slides
//      across and exposes the background) can be INVISIBLE to AO-space
//      checks when both surfaces happen to have similar AO, but the depth
//      jump is unambiguous. exp(-zDiff * 50) gives a smooth falloff rather
//      than a hard reject so sub-pixel reprojection noise doesn't flicker.

cbuffer TemporalCB : register(b0, space2)
{
    uint  viewportWidth;
    uint  viewportHeight;
    float historyAlpha;       // 0 on first frame; steady-state ~0.95
    float rejectionDiff;      // disocclusion threshold; ~0.05 on denoised input
    float4 _pad;              // HLSL cbuffer arrays pad to 16B/element —
                              //    float4 keeps CB size in sync with the C++
                              //    mirror's `float _pad[4]`.
};

Texture2D<float>   gAOInput      : register(t0, space2);  // denoised AO
Texture2D<float>   gEdges        : register(t1, space2);  // packed 4-edge mask
Texture2D<float>   gHistory      : register(t2, space2);  // AO history
Texture2D<float>   gPrevDepth    : register(t3, space2);  // prev linear depth
Texture2D<float2>  gVelocity     : register(t4, space2);
Texture2D<float>   gCurrDepth    : register(t5, space2);  // current linear depth (mip0)
RWTexture2D<float> gOutput       : register(u0, space2);  // AO write
RWTexture2D<float> gPrevDepthOut : register(u1, space2);  // depth copy for next frame
SamplerState       gLinear       : register(s0, space2);

// Unpack the 8-bit edge byte into 4 cardinal flatness values. Written by
// XeGTAO.cs.hlsl :: XeGTAO_PackEdges; semantics: 1.0 = flat (no geometric
// edge), 0.0 = strong depth discontinuity. Quantised to {0, 1/3, 2/3, 1}.
// Mirror of XeGTAODenoise.cs.hlsl :: XeGTAO_UnpackEdges so we don't depend on
// that file's inclusion.
float4 UnpackEdges(float packedVal)
{
    uint pv = uint(packedVal * 255.5);
    float4 e;
    e.x = float((pv >> 6) & 0x03u) / 3.0;
    e.y = float((pv >> 4) & 0x03u) / 3.0;
    e.z = float((pv >> 2) & 0x03u) / 3.0;
    e.w = float((pv >> 0) & 0x03u) / 3.0;
    return saturate(e);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= viewportWidth || dtid.y >= viewportHeight) return;

    const int2   px      = int2(dtid.xy);
    const float2 vpSize  = float2(viewportWidth, viewportHeight);
    const int2   vpMax   = int2(viewportWidth - 1, viewportHeight - 1);
    const float  currAO  = gAOInput.Load(int3(px, 0));
    const float  currZ   = gCurrDepth.Load(int3(px, 0));

    // Stash current depth for next frame's temporal pass, unconditionally —
    // even on the first frame / post-resize path below, we want next frame
    // to have a valid prev-depth source.
    gPrevDepthOut[px] = currZ;

    // First frame / resize — historyAlpha is 0, skip all history work.
    if (historyAlpha <= 0.0)
    {
        gOutput[px] = currAO;
        return;
    }

    // Reproject via GBuffer NDC velocity. velocity.xy = (currNDC - prevNDC).
    // UV-space delta = flip Y (NDC +y up → UV +y down) and scale by 0.5
    // (NDC is [-1,1], UV is [0,1]).
    const float2 currUV  = (float2(px) + 0.5) / vpSize;
    const float2 vel     = gVelocity.Load(int3(px, 0)).xy;
    const float2 uvDelta = float2(vel.x, -vel.y) * 0.5;
    const float2 prevUV  = currUV - uvDelta;

    // Soft screen-edge fade — avoids "has history / no history" snap-flicker
    // for pixels whose reprojection crosses a viewport boundary.
    const float2 edgeDist   = min(prevUV, 1.0 - prevUV);
    const float  edgeDistPx = min(edgeDist.x * vpSize.x,
                                  edgeDist.y * vpSize.y);
    const float  edgeFade   = saturate(edgeDistPx * (1.0 / 20.0));

    // ---- 5-tap cross neighbourhood stats ----------------------------------
    // On the denoised input the 3×3 corners often straddle depth edges (think
    // alcove corners meeting at diagonals), and those corners pull mean/σ in
    // misleading directions. A cross of 5 (center + 4 cardinals) avoids the
    // diagonals and gives a cleaner estimate of the central surface's AO
    // spread. gamma = 1.0 is tight enough on denoised σ to treat any clip as
    // a real disocclusion signal.
    const float n0 = currAO;
    const float n1 = gAOInput.Load(int3(clamp(px + int2(-1,  0), int2(0,0), vpMax), 0));
    const float n2 = gAOInput.Load(int3(clamp(px + int2( 1,  0), int2(0,0), vpMax), 0));
    const float n3 = gAOInput.Load(int3(clamp(px + int2( 0, -1), int2(0,0), vpMax), 0));
    const float n4 = gAOInput.Load(int3(clamp(px + int2( 0,  1), int2(0,0), vpMax), 0));

    const float aoMean = (n0 + n1 + n2 + n3 + n4) * (1.0 / 5.0);
    const float aoVar  = max(0.0,
        ((n0 - aoMean) * (n0 - aoMean) +
         (n1 - aoMean) * (n1 - aoMean) +
         (n2 - aoMean) * (n2 - aoMean) +
         (n3 - aoMean) * (n3 - aoMean) +
         (n4 - aoMean) * (n4 - aoMean)) * (1.0 / 5.0));

    // σ floor: the denoised signal is very smooth so σ can drop near zero on
    // uniform surfaces. Small floor (0.02) keeps the clip window from
    // collapsing onto a single value when the neighbourhood happens to be
    // exactly equal, which would cause frame-to-frame mean drift (from the
    // temporal accumulation itself) to read as disocclusion.
    const float kSigmaFloor = 0.02;
    const float aoStdev     = max(sqrt(aoVar), kSigmaFloor);
    const float kGamma      = 1.0;
    const float aoMin       = saturate(aoMean - kGamma * aoStdev);
    const float aoMax       = saturate(aoMean + kGamma * aoStdev);

    // ---- 4-tap history fetch with cross-surface rejection ------------------
    // Bilinear history sampling at depth edges (e.g. alcove walls) mixes AO
    // from two surfaces into a value that sits between them — neither the
    // floor's history nor the wall's. Against the now-tight variance-clip
    // window that mixed value often still squeezes through, so we guard it
    // here by comparing the 4 corner taps: if their range is bigger than the
    // neighbourhood stdev would predict for a coherent surface, pick the
    // single corner whose value is closest to aoMean (= most likely to be on
    // the current pixel's surface). Otherwise bilinear-interpolate normally.
    const float2 prevUVexact = prevUV * vpSize - 0.5;
    const int2   p00         = int2(floor(prevUVexact));
    const float2 fPart       = prevUVexact - float2(p00);

    const int2 q00 = clamp(p00 + int2(0, 0), int2(0, 0), vpMax);
    const int2 q10 = clamp(p00 + int2(1, 0), int2(0, 0), vpMax);
    const int2 q01 = clamp(p00 + int2(0, 1), int2(0, 0), vpMax);
    const int2 q11 = clamp(p00 + int2(1, 1), int2(0, 0), vpMax);

    const float h00 = gHistory.Load(int3(q00, 0));
    const float h10 = gHistory.Load(int3(q10, 0));
    const float h01 = gHistory.Load(int3(q01, 0));
    const float h11 = gHistory.Load(int3(q11, 0));

    const float hRange = max(max(h00, h10), max(h01, h11))
                       - min(min(h00, h10), min(h01, h11));
    const float crossSurfaceThresh = max(0.08, aoStdev * 3.0);

    float historyAO;
    if (hRange > crossSurfaceThresh)
    {
        float best     = h00;
        float bestDist = abs(h00 - aoMean);
        float d;
        d = abs(h10 - aoMean); if (d < bestDist) { bestDist = d; best = h10; }
        d = abs(h01 - aoMean); if (d < bestDist) { bestDist = d; best = h01; }
        d = abs(h11 - aoMean); if (d < bestDist) { bestDist = d; best = h11; }
        historyAO = best;
    }
    else
    {
        historyAO = lerp(lerp(h00, h10, fPart.x),
                         lerp(h01, h11, fPart.x),
                         fPart.y);
    }

    // ---- Variance clip + clamp-delta rejection -----------------------------
    const float historyClamped = clamp(historyAO, aoMin, aoMax);
    const float clampDelta     = abs(historyAO - historyClamped);
    const float rejection      = smoothstep(rejectionDiff * 0.5,
                                            rejectionDiff, clampDelta);

    // ---- Edge-gated rejection boost ---------------------------------------
    // Edge pixels are spatial-denoise dead-zones: XeGTAODenoise's bilateral
    // weights collapse to near zero across strong geometric edges, so edge
    // pixels carry their raw (un-smoothed) per-frame AO into this pass. The
    // ONLY mechanism left to clean up that noise is temporal integration at
    // α≈0.95 — a blanket edge-α drop there (which an earlier revision tried)
    // starves the integrator of history and the raw noise becomes visible.
    //
    // Instead, let flat AND edge pixels both use the full α baseline and
    // trust the variance-clip rejection above to pull α down ONLY when a
    // real disocclusion shows up (clampDelta > rejectionDiff). To make that
    // rejection slightly more trigger-happy on edges (disocclusion is more
    // likely there), we amplify the existing rejection signal on flagged
    // edges instead of subtracting from α directly. On static edges,
    // rejection stays at 0, so this has no effect — no noise re-introduced.
    const float4 edges    = UnpackEdges(gEdges.Load(int3(px, 0)));
    const float  flatness = dot(edges, 0.25.xxxx);
    const float  edgeBoost = lerp(1.5, 1.0, flatness);     // 1×..1.5× multiplier
    const float  rejectionE = saturate(rejection * edgeBoost);

    // ---- Plane-distance disocclusion rejection (Fix 3) --------------------
    // AO-space rejection cannot see disocclusion when the revealed surface
    // happens to have similar AO to the obstructing surface — a dynamic
    // object (character hand, swinging door) sliding across the camera
    // leaves exactly that kind of ghost. Depth is immune: sample prev-frame
    // linear Z at the reprojected UV and compare against current Z at the
    // same pixel. A single smooth falloff — exp(-zDiff · 50) — gives full
    // acceptance when the relative Z error is under ~1%, falls off over a
    // few percent, and reads as near-zero beyond ~8% (which is where two
    // distinct surfaces at normal depth precision start to live).
    //
    // The /max(currZ, 1e-4) normalises by depth so the threshold is
    // scale-invariant — a 1-unit Z change is HUGE close to the camera, but
    // noise-level at 100 units away. Multiplier 50 is the classic TAA depth
    // rejection constant; tighter values over-reject on sub-pixel
    // reprojection noise, looser values let ghosts through.
    const float prevZ        = gPrevDepth.SampleLevel(gLinear, saturate(prevUV), 0);
    const float zDiff        = abs(currZ - prevZ) / max(currZ, 1e-4);
    const float depthAccept  = exp(-zDiff * 50.0);

    const float alpha = historyAlpha * edgeFade * (1.0 - rejectionE) * depthAccept;

    gOutput[px] = lerp(currAO, historyClamped, alpha);
}
