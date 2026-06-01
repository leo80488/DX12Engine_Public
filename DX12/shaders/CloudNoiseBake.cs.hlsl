// CloudNoiseBake.cs.hlsl
// -----------------------------------------------------------------------------
// One-shot bake of a 128^3 tileable cloud noise volume.
// Combines multi-octave Worley (FBM-style) with low-frequency Perlin so the
// shape has both billowy puffs (Worley) and large-scale variation (Perlin).
// Output is single-channel R8 in [0,1].
//
// Dispatched once in CloudPass::Execute on first frame, then sampled every
// frame by CloudRaymarch.cs.hlsl.
// -----------------------------------------------------------------------------

RWTexture3D<float> NoiseOut : register(u0, space2);

// ---------------------------------------------------------------------------
// Tileable hash — returns a deterministic feature-point offset in [0,1]^3 for
// each integer cell. Tiling is achieved by wrapping the cell index mod gridDim.
static const float kGridDim = 4.0;  // 4 cells across the texture for base Worley

float3 Hash3(int3 c, int3 gridDim)
{
    c = ((c % gridDim) + gridDim) % gridDim;
    float3 p = float3(c) + 0.5;
    p = frac(p * float3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return frac(float3((p.x + p.y) * p.z,
                       (p.y + p.z) * p.x,
                       (p.z + p.x) * p.y));
}

// Worley distance in [0,1]: 0 = at a feature point, 1 = far away.
float Worley(float3 p, int gridDim)
{
    p *= float(gridDim);
    int3 ip = (int3)floor(p);
    float3 fp = frac(p);

    float minDist = 1e9;
    [unroll] for (int x = -1; x <= 1; ++x)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int z = -1; z <= 1; ++z)
    {
        int3 c = ip + int3(x, y, z);
        float3 feature = float3(x, y, z) + Hash3(c, int3(gridDim, gridDim, gridDim));
        float3 r = feature - fp;
        minDist = min(minDist, dot(r, r));
    }
    return saturate(sqrt(minDist));
}

// 3-octave Worley FBM (inverted so high values = inside a puff).
float WorleyFBM(float3 p)
{
    float w  = (1.0 - Worley(p,  4)) * 0.625;
    w       += (1.0 - Worley(p,  8)) * 0.250;
    w       += (1.0 - Worley(p, 16)) * 0.125;
    return saturate(w);
}

// Low-frequency Perlin-ish (smooth value noise) for shape variation.
float Hash1(int3 c, int gridDim)
{
    c = ((c % gridDim) + gridDim) % gridDim;
    float3 p = float3(c);
    p = frac(p * float3(127.1, 311.7, 74.7));
    return frac(sin(dot(p, float3(12.9898, 78.233, 39.425))) * 43758.5453);
}

float ValueNoise(float3 p, int gridDim)
{
    p *= float(gridDim);
    int3 ip = (int3)floor(p);
    float3 fp = frac(p);
    fp = fp * fp * (3.0 - 2.0 * fp);   // smoothstep

    float v000 = Hash1(ip + int3(0,0,0), gridDim);
    float v100 = Hash1(ip + int3(1,0,0), gridDim);
    float v010 = Hash1(ip + int3(0,1,0), gridDim);
    float v110 = Hash1(ip + int3(1,1,0), gridDim);
    float v001 = Hash1(ip + int3(0,0,1), gridDim);
    float v101 = Hash1(ip + int3(1,0,1), gridDim);
    float v011 = Hash1(ip + int3(0,1,1), gridDim);
    float v111 = Hash1(ip + int3(1,1,1), gridDim);

    float v00 = lerp(v000, v100, fp.x);
    float v10 = lerp(v010, v110, fp.x);
    float v01 = lerp(v001, v101, fp.x);
    float v11 = lerp(v011, v111, fp.x);
    float v0  = lerp(v00,  v10,  fp.y);
    float v1  = lerp(v01,  v11,  fp.y);
    return lerp(v0, v1, fp.z);
}

// Remap helper — Schneider style.
float Remap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) * (newHi - newLo) / max(1e-5, hi - lo);
}

[numthreads(8, 8, 8)]
void main(uint3 dt : SV_DispatchThreadID)
{
    // 0..1 normalised position inside the texture (tileable).
    const float3 uvw = (float3(dt) + 0.5) / 128.0;

    // Base Worley puffs.
    float worley = WorleyFBM(uvw);

    // Low-freq Perlin to add cumulus shape variation.
    float perlin = ValueNoise(uvw, 4);

    // Schneider-style Perlin-Worley combine: remap worley to fall off
    // outside the Perlin pocket. The result has clear puff cores with
    // gradual edges.
    float density = Remap(worley, 1.0 - perlin, 1.0, 0.0, 1.0);

    NoiseOut[dt] = saturate(density);
}
