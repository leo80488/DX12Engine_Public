// SpecularPrefilter.cs.hlsl
// -----------------------------------------------------------------------------
// GGX importance-sampled pre-filter for specular IBL (split-sum approximation).
// Given a source TextureCube (raw environment radiance) produces one mip of a
// RWTexture2DArray output cubemap. Roughness is derived from the mip level.
//
// Invocation per mip level:
//   Dispatch((faceW+7)/8, (faceW+7)/8, 6)   with one thread-group per 8×8 tile.
//   DTid.z selects face (0..5) — output is Texture2DArray with 6 slices.
//
// Compute root sig (shared — see GraphicsDX12::CreateComputeRootSignature):
//   [0] CBV  b0 space2 — PrefilterCB
//   [1] SRV  t0 space2 — source TextureCube<float4>
//   [4] UAV  u0 space2 — output RWTexture2DArray<float4> (one mip)
//   static sampler s0 space2 — linear clamp
// -----------------------------------------------------------------------------

#define PI 3.14159265359

cbuffer PrefilterCB : register(b0, space2)
{
    uint  FaceSize;      // output face edge (e.g. 128 >> mip)
    uint  SampleCount;   // usually 1024 (lower for mip 0 where roughness≈0)
    float Roughness;     // mip-based roughness in [0, 1]
    uint  SourceMipCount;// mip count of source cubemap (for lod-sampling skirt)
    uint  FaceIndex;     // which cube face this dispatch writes (0..5)
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

TextureCube<float4>       SrcEnv    : register(t0, space2);
RWTexture2DArray<float4>  DstFace   : register(u0, space2);
SamplerState              LinClamp  : register(s0, space2);

// ---- Hammersley / GGX helpers (standard split-sum split) --------------------
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
float2 Hammersley(uint i, uint N) { return float2(float(i) / float(N), RadicalInverseVdC(i)); }

float3 ImportanceSampleGGX(float2 xi, float3 N, float a)
{
    float phi      = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));

    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    // Tangent-space basis aligned with N.
    float3 up    = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T     = normalize(cross(up, N));
    float3 B     = cross(N, T);
    return normalize(T * H.x + B * H.y + N * H.z);
}

float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float d  = (NdotH * a2 - NdotH) * NdotH + 1.0;
    return a2 / (PI * d * d + 1e-7);
}

// ---- Cubemap face index → direction ----------------------------------------
// D3D11/12 cubemap face conventions:
//   0: +X   1: -X   2: +Y   3: -Y   4: +Z   5: -Z
float3 FaceUVToDir(uint face, float2 uv)
{
    // uv ∈ [-1, +1]
    float3 d;
    [branch] switch (face)
    {
        case 0: d = float3( 1.0, -uv.y, -uv.x); break;  // +X
        case 1: d = float3(-1.0, -uv.y,  uv.x); break;  // -X
        case 2: d = float3( uv.x,  1.0,  uv.y); break;  // +Y
        case 3: d = float3( uv.x, -1.0, -uv.y); break;  // -Y
        case 4: d = float3( uv.x, -uv.y,  1.0); break;  // +Z
        default: d = float3(-uv.x, -uv.y, -1.0); break; // -Z
    }
    return normalize(d);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= FaceSize || DTid.y >= FaceSize) return;

    // Temporal prefilter: each dispatch handles ONE face (selected by CB) so
    // the cost is 1/6 of the "all faces every frame" approach. The face index
    // is written into slice `FaceIndex` of the RWTexture2DArray.
    uint face = FaceIndex;
    // Pixel center UV in [-1, +1].
    float2 uv = (float2(DTid.xy) + 0.5) / float(FaceSize) * 2.0 - 1.0;
    float3 N  = FaceUVToDir(face, uv);

    // Split-sum assumption: V = R = N.
    float3 R = N;
    float3 V = N;

    float a = max(Roughness * Roughness, 1e-3);

    float3 sum = float3(0, 0, 0);
    float  wsum = 0.0;

    // Mip-0 (roughness≈0) case: fall back to the source texel to avoid the full
    // 1024-sample loop (roughness=0 → all weight at the mirror direction).
    if (Roughness < 1e-3)
    {
        sum  = SrcEnv.SampleLevel(LinClamp, N, 0).rgb;
        wsum = 1.0;
    }
    else
    {
        for (uint i = 0; i < SampleCount; ++i)
        {
            float2 xi    = Hammersley(i, SampleCount);
            float3 H     = ImportanceSampleGGX(xi, N, a);
            float3 L     = normalize(2.0 * dot(V, H) * H - V);
            float  NdotL = saturate(dot(N, L));
            if (NdotL <= 0.0) continue;

            // Karis mip-bias: pick a source LOD based on sample density so
            // under-sampled high-roughness regions don't alias (see Krivanek et al.).
            float NdotH = saturate(dot(N, H));
            float VdotH = saturate(dot(V, H));
            float pdf   = D_GGX(NdotH, a) * NdotH / (4.0 * VdotH) + 1e-6;
            float saTex = 4.0 * PI / (6.0 * float(FaceSize) * float(FaceSize));
            float saSam = 1.0 / (float(SampleCount) * pdf);
            float lod   = 0.5 * log2(saSam / saTex);
            lod = clamp(lod, 0.0, float(max(int(SourceMipCount) - 1, 0)));

            sum  += SrcEnv.SampleLevel(LinClamp, L, lod).rgb * NdotL;
            wsum += NdotL;
        }
        sum /= max(wsum, 1e-4);
    }

    DstFace[uint3(DTid.xy, face)] = float4(sum, 1.0);
}
