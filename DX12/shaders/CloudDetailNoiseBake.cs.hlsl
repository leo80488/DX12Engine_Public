// CloudDetailNoiseBake.cs.hlsl
// -----------------------------------------------------------------------------
// One-shot bake of the 32^3 tileable DETAIL (erosion) volume -- three Worley
// FBM bands at rising frequency (Schneider 2015 layout, RGB):
//
//   R = Worley FBM, cells 2 / 4 /  8   (weights .625/.25/.125)
//   G = Worley FBM, cells 4 / 8 / 16
//   B = Worley FBM, cells 8 / 16       (weights .75/.25)
//
// The raymarch combines RGB into an FBM and erodes the base shape's EDGES
// only: wispy (inverted) near the cloud base, billowy near the top.
// -----------------------------------------------------------------------------

RWTexture3D<float4> NoiseOut : register(u0, space2);

static const float kDim = 32.0;

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

float WorleyBillow(float3 p, int gridDim) { return 1.0 - Worley(p, gridDim); }

[numthreads(4, 4, 4)]
void main(uint3 dt : SV_DispatchThreadID)
{
    const float3 uvw = (float3(dt) + 0.5) / kDim;

    float r = WorleyBillow(uvw, 2) * 0.625
            + WorleyBillow(uvw, 4) * 0.25
            + WorleyBillow(uvw, 8) * 0.125;
    float g = WorleyBillow(uvw,  4) * 0.625
            + WorleyBillow(uvw,  8) * 0.25
            + WorleyBillow(uvw, 16) * 0.125;
    float b = WorleyBillow(uvw,  8) * 0.75
            + WorleyBillow(uvw, 16) * 0.25;

    NoiseOut[dt] = float4(saturate(r), saturate(g), saturate(b), 1.0);
}
