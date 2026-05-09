// CAS.cs.hlsl — AMD FidelityFX Contrast Adaptive Sharpening (HDR adapted).
//
// Runs post-TAA / pre-tonemap on the HDR scene. LDS-optimised: each 8×8
// threadgroup cooperatively loads a 10×10 halo tile (100 texels) into
// groupshared instead of issuing 9 Loads per thread (576 texels per group).
// That's ~5.8× fewer texture reads; the rest of the work stays per-thread.
//
// Reference: FidelityFX-SDK CAS 1.0, ffx_cas.h. Port notes:
//   * LDR path only — we don't use the LINEAR sample halving trick because
//     8×8 tiles with 1-px halo already fit comfortably in LDS.
//   * Amp formula keeps AMD's "headroom" semantics (preserves the LDR
//     behaviour identically) and adds an HDR safety clamp so bright pixels
//     (lumaMax > 2) cleanly zero out sharpening instead of over-amplifying.
//
// Compute root signature (space2):
//   b0 — CASCB { uint width, height; float sharpness; float _pad; }
//   t0 — HDR input  (R16G16B16A16_FLOAT)
//   u0 — HDR output (R16G16B16A16_FLOAT)

cbuffer CASCB : register(b0, space2)
{
    uint  width;
    uint  height;
    float sharpness;   // [0..1] — 0 = subtle (AMD's min), 1 = maximum
    float _pad;
};

Texture2D<float4>   gInput  : register(t0, space2);
RWTexture2D<float4> gOutput : register(u0, space2);

// ---- LDS tile ----------------------------------------------------------
// 8×8 threadgroup with 1-pixel halo ⇒ 10×10 float4 tile = 1600 bytes LDS.
// Comfortable fit; occupancy is not LDS-limited on any SM we target.
#define TILE       8
#define HALO       1
#define TILE_WH    (TILE + 2 * HALO)       // 10
#define TILE_AREA  (TILE_WH * TILE_WH)     // 100

groupshared float4 g_tile[TILE_WH][TILE_WH];

[numthreads(TILE, TILE, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID,
            uint3 gtid : SV_GroupThreadID,
            uint3 gid  : SV_GroupID)
{
    const int2 maxPx = int2(int(width) - 1, int(height) - 1);

    // ---- Cooperative load: 64 threads × up to 2 iters fill 100 slots ------
    // Slots 0..63 covered by iter 0 (every thread); slots 64..99 covered by
    // iter 1 (first 36 threads). Out-of-viewport taps are clamped to edge so
    // the border doesn't leak uninitialised values.
    const int2 groupOrigin = int2(gid.xy) * TILE - HALO;
    const uint linearIdx   = gtid.y * TILE + gtid.x;   // 0..63

    {
        const uint slot = linearIdx;                   // 0..63
        const uint ty   = slot / TILE_WH;
        const uint tx   = slot % TILE_WH;
        const int2 srcPx = clamp(groupOrigin + int2(tx, ty), int2(0, 0), maxPx);
        g_tile[ty][tx]   = gInput.Load(int3(srcPx, 0));
    }
    if (linearIdx < (TILE_AREA - TILE * TILE))         // 100 - 64 = 36
    {
        const uint slot = linearIdx + TILE * TILE;     // 64..99
        const uint ty   = slot / TILE_WH;
        const uint tx   = slot % TILE_WH;
        const int2 srcPx = clamp(groupOrigin + int2(tx, ty), int2(0, 0), maxPx);
        g_tile[ty][tx]   = gInput.Load(int3(srcPx, 0));
    }

    GroupMemoryBarrierWithGroupSync();

    // Bail on out-of-viewport threads AFTER the cooperative load — they
    // still had to contribute to LDS for their in-bounds neighbours above.
    if (dtid.x >= width || dtid.y >= height) return;

    // ---- 3×3 read from LDS ------------------------------------------------
    //   a b c
    //   d e f       (e = centre)
    //   g h i
    const int lx = int(gtid.x) + HALO;
    const int ly = int(gtid.y) + HALO;

    const float4 a = g_tile[ly - 1][lx - 1];
    const float4 b = g_tile[ly - 1][lx    ];
    const float4 c = g_tile[ly - 1][lx + 1];
    const float4 d = g_tile[ly    ][lx - 1];
    const float4 e = g_tile[ly    ][lx    ];
    const float4 f = g_tile[ly    ][lx + 1];
    const float4 g = g_tile[ly + 1][lx - 1];
    const float4 h = g_tile[ly + 1][lx    ];
    const float4 i = g_tile[ly + 1][lx + 1];

    // Full 3×3 min/max, per channel. Corners are included for range clamping.
    const float3 minRGB = min(min(min(a.rgb, b.rgb), min(c.rgb, d.rgb)),
                              min(min(e.rgb, f.rgb), min(min(g.rgb, h.rgb), i.rgb)));
    const float3 maxRGB = max(max(max(a.rgb, b.rgb), max(c.rgb, d.rgb)),
                              max(max(e.rgb, f.rgb), max(max(g.rgb, h.rgb), i.rgb)));

    // ---- Amp: AMD headroom formula + HDR safety ---------------------------
    // Original LDR formula:
    //   amp = saturate( min(lumaMin, 2.0 - lumaMax) / lumaMax )
    // Reads as "how much headroom do we have before we'd clip at either
    // end of LDR?". On HDR inputs lumaMax can exceed 2 → `2 - lumaMax`
    // goes negative → saturate clamps the result to 0 (no sharpening),
    // which is the right call for bright emissives / fireflies where
    // amplification would just make outliers worse.
    //
    // We use the same structure but clamp `2 - lumaMax` to ≥ 0 explicitly
    // so the formula reads intentionally and avoids a negative intermediate
    // that multiplied through rcp could look like a subtle bug.
    const float3 lumaW   = float3(0.2126, 0.7152, 0.0722);
    const float  lumaMin = dot(minRGB, lumaW);
    const float  lumaMax = dot(maxRGB, lumaW);

    const float headroom = max(0.0, 2.0 - lumaMax);
    const float rcp      = 1.0 / max(lumaMax, 1e-4);
    float amp = saturate(min(lumaMin, headroom) * rcp);
    amp = sqrt(amp);   // AMD's perceptual curve

    // Weight peak interpolates from -1/8 (sharpness=0, subtle) to -1/5
    // (sharpness=1, maximum). Negative because the cross filter subtracts
    // neighbours to sharpen.
    const float peak = -1.0 / lerp(8.0, 5.0, sharpness);
    const float w    = amp * peak;
    const float rcpW = 1.0 / (1.0 + 4.0 * w);

    // 5-tap cross sharpen. Factored so neighbours share a single weight
    // multiplication. Alpha passes through from the centre tap unchanged.
    const float3 sharpened = (b.rgb + d.rgb + f.rgb + h.rgb) * (w * rcpW)
                           + e.rgb * rcpW;

    gOutput[int2(dtid.xy)] = float4(max(sharpened, 0.0), e.a);
}
