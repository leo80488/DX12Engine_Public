// Shadow.ps.hlsl — depth-only PS for shadow map rendering.
// ALPHA_TEST permutation samples the bindless albedo and clips on alphaRef
// (matches GBuffer.ps's bindless path so foliage shadows align with main pass).

#include "material.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

struct PSIn
{
    float4 sv : SV_POSITION;
    float2 uv : TEXCOORD0;
};

#if ALPHA_TEST

StructuredBuffer<MaterialGPUData> g_Materials    : register(t2, space0);
Texture2D                          g_AllTextures[] : register(t0, space2);  // bindless table
SamplerState                       g_LinearWrap  : register(s0, space0);

void main(PSIn i)
{
    MaterialGPUData mat = g_Materials[materialIndex];
    int texBaseColor = mat.textureHandleIds[0];   // BASECOLORMAP
    // No texture bound → treat as fully opaque (don't clip everything to nothing).
    if (texBaseColor < 0) return;
    float alpha = g_AllTextures[texBaseColor].Sample(g_LinearWrap, i.uv).a;
    clip(alpha - mat.alphaRef);
}

#else

void main(PSIn i) {}

#endif
