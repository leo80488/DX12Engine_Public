// SkySHProjection.cs.hlsl
// -----------------------------------------------------------------------------
// Projects a source environment TextureCube into L2 spherical harmonics
// (9 float3 coefficients). Intended to be dispatched once per frame so the SH
// coefficients track a dynamic sky.
//
// Invocation: Dispatch(1, 1, 1) with one 256-thread group.
//   Thread i samples direction d_i from a stratified/Hammersley sequence,
//   reads the cubemap, accumulates sky * basis * weight into LDS,
//   group-reduces (256 -> 1), and thread 0 writes 9 entries to the output buffer.
//
// IMPORTANT: the source cubemap here is interpreted as a spherical function
// (irradiance or radiance — the shader is agnostic). The evaluator in the PS
// reconstructs that function at direction N via a plain SH dot product.
//
// Compute root sig (reused from BloomPass — see CreateComputeRootSignature in
// GraphicsDX12.cpp):
//   [0] CBV  b0 space2 — SHConstants
//   [1] SRV  t0 space2 — source TextureCube
//   [4] UAV  u0 space2 — RWStructuredBuffer<float4> (9 entries, .xyz = SH, .w = 0)
//   static sampler  s0 space2 — linear clamp
// -----------------------------------------------------------------------------

#define PI 3.14159265359

cbuffer SHConstants : register(b0, space2)
{
    uint  SampleCount;   // total samples to take (multiple of 256)
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

TextureCube<float4>         SrcEnv    : register(t0, space2);
RWStructuredBuffer<float4>  SHOut     : register(u0, space2);
SamplerState                LinClamp  : register(s0, space2);

// Hammersley 2D LDS (radical-inverse base-2 + index/N)
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}
float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverseVdC(i));
}

// Uniform unit-sphere sample (not hemisphere — sky is full-sphere)
float3 SampleSphere(float2 xi)
{
    float z   = 1.0 - 2.0 * xi.x;
    float r   = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * PI * xi.y;
    return float3(r * cos(phi), z, r * sin(phi));
}

// L2 real SH basis (9 coefficients), direction d should be unit.
void EvalSHBasis(float3 d, out float b[9])
{
    // L0
    b[0] = 0.282095;
    // L1
    b[1] = 0.488603 * d.y;
    b[2] = 0.488603 * d.z;
    b[3] = 0.488603 * d.x;
    // L2
    b[4] = 1.092548 * d.x * d.y;
    b[5] = 1.092548 * d.y * d.z;
    b[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
    b[7] = 1.092548 * d.z * d.x;
    b[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

#define THREADS 256
groupshared float3 g_SH[9][THREADS];

[numthreads(THREADS, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint GI : SV_GroupIndex)
{
    // Each thread takes `stride` samples (SampleCount / 256).
    uint stride = max(SampleCount / THREADS, 1u);

    float3 accum[9];
    [unroll] for (int k = 0; k < 9; ++k) accum[k] = float3(0, 0, 0);

    // Monte Carlo integration: for each sample, add sky(d) * basis_i(d) / pdf.
    // For a uniform sphere sample pdf = 1/(4*pi), so weight per sample is
    // (4*pi) / SampleCount. We fold that into the final normalisation.
    for (uint s = 0; s < stride; ++s)
    {
        uint   idx = GI * stride + s;
        float2 xi  = Hammersley(idx, SampleCount);
        float3 dir = SampleSphere(xi);

        // Sample the environment cubemap at mip 0.
        float3 sky = SrcEnv.SampleLevel(LinClamp, dir, 0.0).rgb;

        float basis[9];
        EvalSHBasis(dir, basis);

        [unroll] for (int i = 0; i < 9; ++i)
            accum[i] += sky * basis[i];
    }

    [unroll] for (int i = 0; i < 9; ++i)
        g_SH[i][GI] = accum[i];

    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction 256 -> 1.
    [unroll] for (uint off = THREADS / 2; off > 0; off >>= 1)
    {
        if (GI < off)
        {
            [unroll] for (int i = 0; i < 9; ++i)
                g_SH[i][GI] += g_SH[i][GI + off];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (GI == 0)
    {
        float norm = (4.0 * PI) / float(SampleCount);
        [unroll] for (int i = 0; i < 9; ++i)
            SHOut[i] = float4(g_SH[i][0] * norm, 0.0);
    }
}
