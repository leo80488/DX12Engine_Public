// CloudNoiseBake.cs.hlsl
// -----------------------------------------------------------------------------
// One-shot bake of the 128^3 tileable BASE-SHAPE volume, Schneider/Nubis layout
// (SIGGRAPH 2015 "Real-Time Volumetric Cloudscapes of Horizon Zero Dawn"):
//
//   R = Perlin-Worley  -- tileable gradient-Perlin FBM remapped over a Worley
//                        FBM so Worley billows fill Perlin's low-density holes.
//   G = Worley FBM, cell counts  4 /  8 / 16   (weights .625/.25/.125)
//   B = Worley FBM, cell counts  8 / 16 / 32
//   A = Worley FBM, cell counts 16 / 32        (weights .75/.25)
//
// The raymarch dilates R by the GBA FBM:  Remap(R, fbm-1, 1, 0, 1) -- erosion
// by remap, NOT multiplication, so cloud cores stay opaque (Nubis 2017).
//
// Dispatched once in CloudPass::Execute on first frame.
// -----------------------------------------------------------------------------

RWTexture3D<float4> NoiseOut : register(u0, space2);

static const float kDim = 128.0;

// ---------------------------------------------------------------------------
// Tileable hashes -- wrap the integer cell index mod gridDim so the noise
// tiles exactly across the texture.
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

// Gradient direction for tileable Perlin: hash -> unit-ish vector in [-1,1]^3.
float3 GradDir(int3 c, int3 gridDim)
{
    return normalize(Hash3(c, gridDim) * 2.0 - 1.0 + 1e-4);
}

// ---------------------------------------------------------------------------
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

// Inverted Worley -- 1 at the cell core so cells read as billows.
float WorleyBillow(float3 p, int gridDim) { return 1.0 - Worley(p, gridDim); }

// ---------------------------------------------------------------------------
// Tileable 3D gradient (Perlin) noise, output in [-1,1].
float Perlin(float3 p, int gridDim)
{
    p *= float(gridDim);
    int3 ip = (int3)floor(p);
    float3 fp = frac(p);
    // Quintic fade -- C2-continuous derivative (classic improved Perlin).
    float3 u = fp * fp * fp * (fp * (fp * 6.0 - 15.0) + 10.0);

    int3 gd = int3(gridDim, gridDim, gridDim);
    float v000 = dot(GradDir(ip + int3(0,0,0), gd), fp - float3(0,0,0));
    float v100 = dot(GradDir(ip + int3(1,0,0), gd), fp - float3(1,0,0));
    float v010 = dot(GradDir(ip + int3(0,1,0), gd), fp - float3(0,1,0));
    float v110 = dot(GradDir(ip + int3(1,1,0), gd), fp - float3(1,1,0));
    float v001 = dot(GradDir(ip + int3(0,0,1), gd), fp - float3(0,0,1));
    float v101 = dot(GradDir(ip + int3(1,0,1), gd), fp - float3(1,0,1));
    float v011 = dot(GradDir(ip + int3(0,1,1), gd), fp - float3(0,1,1));
    float v111 = dot(GradDir(ip + int3(1,1,1), gd), fp - float3(1,1,1));

    float v00 = lerp(v000, v100, u.x);
    float v10 = lerp(v010, v110, u.x);
    float v01 = lerp(v001, v101, u.x);
    float v11 = lerp(v011, v111, u.x);
    float v0  = lerp(v00,  v10,  u.y);
    float v1  = lerp(v01,  v11,  u.y);
    return lerp(v0, v1, u.z);
}

// 3-octave Perlin FBM -> [0,1].
float PerlinFBM(float3 p, int baseFreq)
{
    float n = Perlin(p, baseFreq)      * 0.625
            + Perlin(p, baseFreq * 2)  * 0.25
            + Perlin(p, baseFreq * 4)  * 0.125;
    return saturate(n * 0.5 + 0.5);
}

// Remap helper -- Schneider style.
float Remap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) * (newHi - newLo) / max(1e-5, hi - lo);
}

[numthreads(8, 8, 8)]
void main(uint3 dt : SV_DispatchThreadID)
{
    const float3 uvw = (float3(dt) + 0.5) / kDim;

    // ---- R: Perlin-Worley ---------------------------------------------------
    float perlin = PerlinFBM(uvw, 4);
    float wfbmR  = WorleyBillow(uvw,  8) * 0.625
                 + WorleyBillow(uvw, 16) * 0.25
                 + WorleyBillow(uvw, 32) * 0.125;
    // Worley billows fill the low-density regions of Perlin (Nubis 2017 p34).
    float pw = saturate(Remap(perlin, 0.0, 1.0, wfbmR, 1.0));

    // ---- GBA: Worley FBM octaves at rising frequency ------------------------
    float g = WorleyBillow(uvw,  4) * 0.625
            + WorleyBillow(uvw,  8) * 0.25
            + WorleyBillow(uvw, 16) * 0.125;
    float b = WorleyBillow(uvw,  8) * 0.625
            + WorleyBillow(uvw, 16) * 0.25
            + WorleyBillow(uvw, 32) * 0.125;
    float a = WorleyBillow(uvw, 16) * 0.75
            + WorleyBillow(uvw, 32) * 0.25;

    NoiseOut[dt] = float4(pw, saturate(g), saturate(b), saturate(a));
}
