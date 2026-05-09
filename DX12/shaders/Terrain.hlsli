#ifndef TERRAIN_HLSLI
#define TERRAIN_HLSLI

// Cubic B-spline filtered heightmap sample — 4 bilinear lookups.
//
// Bilinear filtering is C0 continuous but C1-discontinuous: the slope
// changes at every texel boundary. With a low-precision heightmap (BC1/BC4
// compressed → ~8 distinct levels per 4×4 block, or 8-bit grayscale →
// 256 levels) and a large heightScale, those slope discontinuities show
// up as the visible "contour-line" terraces between flat plateaus.
//
// A C2-continuous cubic B-spline filter lets us interpolate smoothly
// through quantised values, hiding the underlying step structure. The
// 4-bilinear-tap formulation is standard (Sigg & Hadwiger 2005, also
// GPU Pro 2 ch. 5) and costs only marginally more than a single bilinear.
//
// uv         — heightmap UV
// uvTexelSize — 1 / heightmap resolution (in UV space)
float TerrainSampleHeightBicubic(Texture2D<float> tex, SamplerState samp,
                                 float2 uv, float uvTexelSize)
{
    float  texSize  = 1.0 / max(uvTexelSize, 1e-6);
    float2 sample   = uv * texSize;
    float2 tex1     = floor(sample - 0.5) + 0.5;
    float2 f        = sample - tex1;

    // Cubic B-spline weights (Mitchell-Netravali B=1, C=0)
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0/6.0) * (-f3 + 3.0 * f2 - 3.0 * f + 1.0);
    float2 w1 = (1.0/6.0) * (3.0 * f3 - 6.0 * f2 + 4.0);
    float2 w2 = (1.0/6.0) * (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0);
    float2 w3 = (1.0/6.0) * f3;

    float2 g0 = w0 + w1;
    float2 g1 = w2 + w3;
    // Bilinear-sample offsets per Sigg & Hadwiger 2005 §3.2:
    //   h0 = w1/g0 - 1  (covers texels at offset -1, 0)
    //   h1 = w3/g1 + 1  (covers texels at offset +1, +2)
    // Using w2 in h1 instead of w3 causes the second tap to read at the
    // wrong texel position; the error is f-dependent so it shows up as
    // periodic wave-shaped bands at texel-boundary frequency.
    float2 h0 = (w1 / g0) - 1.0;
    float2 h1 = (w3 / g1) + 1.0;

    float2 uv0 = (tex1 + h0) / texSize;
    float2 uv1 = (tex1 + h1) / texSize;

    float a = tex.SampleLevel(samp, float2(uv0.x, uv0.y), 0);
    float b = tex.SampleLevel(samp, float2(uv1.x, uv0.y), 0);
    float c = tex.SampleLevel(samp, float2(uv0.x, uv1.y), 0);
    float d = tex.SampleLevel(samp, float2(uv1.x, uv1.y), 0);

    return lerp(lerp(a, b, g1.x), lerp(c, d, g1.x), g1.y);
}

// ---------------------------------------------------------------------------
// Hex-grid stochastic tiling — Heitz & Neyret 2018, "By-Example Procedural
// Stochastic Texturing", simplified to a 3-tap per-pixel blend without the
// full histogram-preservation step.
//
// Why: when terrain layers tile at world-meter scale (e.g. 1 cycle / 2m),
// a regular grid produces visible row/column repetition — the eye picks up
// the periodic pattern. Hex-grid stochastic tiling breaks the lattice by
// rotating + offsetting the texture randomly inside each hexagon cell, then
// blending the 3 nearest cells with barycentric weights so the seam is
// invisible.
//
// Cost: 3× the texture samples vs. plain Sample(), but the lattice goes
// away. Apply only to the most visually-repetitive maps (albedo, normal);
// leave low-frequency maps (ARM, displacement) at 1-tap to keep the
// budget reasonable.
//
// Tile size = 1 unit in input UV space → choose layer.tilingScale so 1
// hex cell ≈ 0.5..1× the texture's repeat distance for clean blending.
// ---------------------------------------------------------------------------

float2 TerrainHash22(float2 p)
{
    p = float2(dot(p, float2(127.1, 311.7)),
               dot(p, float2(269.5, 183.3)));
    return frac(sin(p) * 43758.5453123);
}

// Returns three sample UVs, three barycentric weights, AND the per-tap
// (cos, sin) of the forward rotation applied to that tap's UV. Normal-map
// callers use the rotation to back-rotate the sampled tangent-space
// normal into the original (non-rotated) tangent frame before blending —
// otherwise the three normals are in three different rotated tangent
// frames and weighted-sum averages to garbage.
//
// Albedo / scalar callers can ignore the rotation outputs and just
// blend the samples with the weights directly.
void TerrainStochasticTaps(float2 uv,
                           out float2 outUV0,  out float2 outUV1,  out float2 outUV2,
                           out float  outW0,   out float  outW1,   out float  outW2,
                           out float2 outRot0, out float2 outRot1, out float2 outRot2)
{
    // Skew (u,v) into a triangular-grid coordinate so floor() picks the
    // hex-cell vertex and frac() gives barycentric coords inside the
    // half-cell triangle. Constants are the inverse of the 60° hexagon
    // basis (sqrt(3)/2 = 0.86602540378).
    const float2 t  = float2(uv.x - 0.5773502692 * uv.y,
                             uv.y * 1.1547005384);
    const float2 ti = floor(t);
    const float2 tf = t - ti;

    // Two triangles tile the cell (lower-left and upper-right). Barycentric
    // weights sum to 1; a vertex on the diagonal seam gets weight from
    // both triangles, so the blend is C0 across the seam.
    float2 v0, v1, v2;
    float3 bary;
    if (tf.x + tf.y < 1.0)
    {
        v0   = ti;
        v1   = ti + float2(1, 0);
        v2   = ti + float2(0, 1);
        bary = float3(1.0 - tf.x - tf.y, tf.x, tf.y);
    }
    else
    {
        v0   = ti + float2(1, 1);
        v1   = ti + float2(1, 0);
        v2   = ti + float2(0, 1);
        bary = float3(tf.x + tf.y - 1.0, 1.0 - tf.y, 1.0 - tf.x);
    }

    // Per-vertex hash → independent (rotation, offset) per cell.
    float2 h0 = TerrainHash22(v0);
    float2 h1 = TerrainHash22(v1);
    float2 h2 = TerrainHash22(v2);

    // Rotate uv around origin (in input UV space) by hash-derived angle,
    // then add the hash-derived translation. The rotation is the part
    // that actually destroys row/column repetition; offset alone leaves
    // a visible "shift" pattern.
    float a0 = h0.x * 6.2831853;     // 2π
    float a1 = h1.x * 6.2831853;
    float a2 = h2.x * 6.2831853;
    outRot0 = float2(cos(a0), sin(a0));
    outRot1 = float2(cos(a1), sin(a1));
    outRot2 = float2(cos(a2), sin(a2));

    outUV0 = float2(outRot0.x * uv.x - outRot0.y * uv.y,
                    outRot0.y * uv.x + outRot0.x * uv.y) + h0;
    outUV1 = float2(outRot1.x * uv.x - outRot1.y * uv.y,
                    outRot1.y * uv.x + outRot1.x * uv.y) + h1;
    outUV2 = float2(outRot2.x * uv.x - outRot2.y * uv.y,
                    outRot2.y * uv.x + outRot2.x * uv.y) + h2;

    // Variance-preserving weights: square the barycentric weights and
    // renormalise so the variance of the blended sample matches a single
    // tap (Heitz-Neyret eq. 8 with simplified histogram). Without this,
    // the smooth 3-way lerp in transition zones over-blurs the texture
    // and the "stochastic" feel is lost.
    float3 w2     = bary * bary;
    float  invSum = rcp(w2.x + w2.y + w2.z);
    outW0 = w2.x * invSum;
    outW1 = w2.y * invSum;
    outW2 = w2.z * invSum;
}

#endif // TERRAIN_HLSLI
