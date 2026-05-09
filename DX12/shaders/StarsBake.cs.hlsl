// StarsBake.cs.hlsl — pre-compute the static star cubemap once at startup.
//
// Replaces the previous per-frame procedural star evaluation in Skybox.ps.hlsl.
// The runtime PS now does a single TextureCube sample + multiplies by the
// horizon / day-night fade — no hash math, no cell traversal, no twinkle
// (which the user disabled). This is several orders of magnitude cheaper
// than evaluating the cell hashes for every screen pixel every frame.
//
// Output: TextureCube (6 array slices), R11G11B10_FLOAT. Dispatched as a
// 2DArray UAV with z = face index.
//
// Star generation algorithm (matches the previous procedural shader):
//   1. Ray direction → 3D cell index (Worley-like cell grid)
//   2. Per-cell hash decides presence (~3 % of cells), star position within
//      the cell, colour temperature, size class.
//   3. Compute angular distance from current texel direction to the star's
//      direction; smoothstep into a small disk.
//   4. Pastel colour palette (5 stops, red→orange→white→cyan→blue).

cbuffer StarsBakeCB : register(b0, space2)
{
    uint   CubeSize;            // texels per face (e.g. 512)
    float  StarDensity;         // grid resolution; ~250 → ~few thousand stars
    float  StarBrightnessBake;  // baked brightness (separate from runtime mul)
    uint   _pad0;
};

RWTexture2DArray<float4> gOutput : register(u0, space2);

#define XE_GTAO_PI 3.14159265358979

// 3-channel hash from a 3D cell key. Cheap and well-distributed.
float3 HashCell3(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// Pastel star palette (matches the previous Skybox.ps StarColor).
float3 StarColor(float t)
{
    float3 cRed    = float3(1.00, 0.78, 0.72);
    float3 cOrange = float3(1.00, 0.90, 0.78);
    float3 cWhite  = float3(1.00, 0.98, 0.95);
    float3 cCyan   = float3(0.86, 0.93, 1.00);
    float3 cBlue   = float3(0.78, 0.86, 1.00);
    if (t < 0.25) return lerp(cRed,    cOrange, t * 4.0);
    if (t < 0.50) return lerp(cOrange, cWhite,  (t - 0.25) * 4.0);
    if (t < 0.75) return lerp(cWhite,  cCyan,   (t - 0.50) * 4.0);
                  return lerp(cCyan,   cBlue,   (t - 0.75) * 4.0);
}

// Cube-face UV → world ray direction. DX12 cubemap face order (slice 0..5):
//   0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z.
// uv ∈ [0,1]² covers one face; we map to [-1,+1]² and pick the face axis.
float3 CubeFaceToDir(uint face, float2 uv)
{
    float u = uv.x * 2.0 - 1.0;
    float v = uv.y * 2.0 - 1.0;
    float3 dir;
    if      (face == 0) dir = float3( 1.0, -v,  -u);  // +X
    else if (face == 1) dir = float3(-1.0, -v,   u);  // -X
    else if (face == 2) dir = float3( u,    1.0, v);  // +Y
    else if (face == 3) dir = float3( u,   -1.0,-v);  // -Y
    else if (face == 4) dir = float3( u,   -v,   1.0);// +Z
    else                dir = float3(-u,   -v,  -1.0);// -Z
    return normalize(dir);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= CubeSize || dtid.y >= CubeSize) return;
    if (dtid.z >= 6) return;

    uint   face = dtid.z;
    float2 uv   = (float2(dtid.xy) + 0.5) / float(CubeSize);
    float3 rd   = CubeFaceToDir(face, uv);

    // ---- Star math (no twinkle, no horizon fade — those stay runtime) ----
    float3 scaled  = rd * StarDensity;
    float3 cellId  = floor(scaled);

    float3 h1 = HashCell3(cellId);
    float3 h2 = HashCell3(cellId + 71.23);

    const float kPresenceThreshold = 0.03;
    float3 colorOut = float3(0, 0, 0);

    float twinklePhase = 0.0;  // 0..1; runtime * 2π → radians. 0 = "no star here".
    if (h1.x <= kPresenceThreshold)
    {
        // Star centre in cell → unit dir.
        float3 starWorld = cellId + h1;
        float3 starDir   = normalize(starWorld);
        float  cosD      = dot(rd, starDir);

        // Per-star size — compromise between "visually small stars" and
        // "enough cubemap texels that bilinear sampling is stable". At
        // 1024² per face one texel ≈ 0.088°, so:
        //   small (~85 %): ~0.18° ≈ 2 texels (cos ≈ 0.999995)
        //   large (~15 %): ~0.35° ≈ 4 texels (cos ≈ 0.999981)
        // After bilinear + tonemap these render as 1-2 screen pixels on
        // 1080p, tight enough that the user doesn't read them as "dots"
        // but wide enough that every screen pixel inside the star hits at
        // least one non-empty cubemap texel (keeps the alpha-phase valid
        // for twinkle).
        float sizeT   = saturate((h2.z - 0.85) / 0.15);
        float coreCos = lerp(0.9999995, 0.9999981, sizeT);

        if (cosD >= coreCos)
        {
            float disk    = smoothstep(coreCos, 1.0, cosD);
            float baseMag = lerp(0.40, 1.40, h1.z);
            float3 col    = StarColor(h1.y);
            // Full-HDR bake (no StarBrightnessBake attenuation here — the
            // runtime shader applies `StarBrightness` once, so the baked
            // value lives near [0.3, 1.4] HDR. This keeps stars in the
            // linear part of the tonemap where brightness modulation (the
            // per-star twinkle) is visually obvious; the old double-
            // attenuation pushed them under the toe of the tonemap so
            // twinkle swing compressed to almost-invisible on-screen.)
            colorOut = col * disk * baseMag;

            // Per-star twinkle phase. Baked into alpha so every cubemap texel
            // inside the SAME star records the SAME phase (h2.x depends only
            // on cellId). Bilinear sampling preserves that flatness within
            // the star's disk footprint, so when runtime does
            //   twinkle = 0.7 + 0.3 * sin(time + alpha * 2π)
            // every pixel of the star brightens/dims in lockstep, which reads
            // as "one object blinking" rather than per-pixel shimmer.
            //
            // Offset by 0.01 so even phase 0 encodes "a star is here" (pure
            // 0 alpha = empty sky — runtime gates twinkle on that).
            twinklePhase = 0.01 + 0.99 * h2.x;
        }
    }

    gOutput[uint3(dtid.xy, face)] = float4(colorOut, twinklePhase);
}
