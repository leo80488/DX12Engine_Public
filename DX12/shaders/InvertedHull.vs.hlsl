// InvertedHull.vs.hlsl — vertex shader for the inverted-hull outline pass.
//
// Extrudes vertices along world-space normals in clip space (pixel-consistent).
// Distance fade: outline width and alpha ramp to zero between fadeStart..fadeEnd.
// Beyond fadeEnd: vertex collapsed behind near plane → triangle clipped by GPU.

#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint outlinePixelsBits;  // asfloat() → per-draw outline thickness in pixels
};

#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);

// Pre-TAA composite: outline rasterizes alongside the main pass at the same
// JITTERED NDC, so TAA reprojects + history-blends both together — outline
// never desyncs from the mesh in motion (Genshin / Honkai approach).
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;       // current, jittered (matches GBuffer raster)
    float4x4 prevViewProj;   // unused here
};

cbuffer OutlineCB : register(b2, space0)
{
    float    outlinePixels;
    float    depthThreshold;
    float    normalThreshold;
    float    outlineStrength;
    float3   outlineColor;
    float    outlineFadeStart;
    uint     vpWidth;
    uint     vpHeight;
    float    outlineFadeEnd;
    float    nearZ;
    float    farZ;
};

struct VSOut
{
    float4 sv       : SV_POSITION;
    float  distFade : TEXCOORD0;
};

VSOut main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md  = MeshDescriptors[meshDescIdx];
    uint           vid = FetchIndex(md, rawID);

    float3   localPos = FETCH_POS(md, vid);
    float3   localNrm = FETCH_NORMAL(md, vid);
    float4x4 world    = InstanceBuffer[instanceOffset + instID].world;

    float4 wPos = mul(float4(localPos, 1.0f), world);
    float3 wNrm = normalize(mul(float4(localNrm, 0.0f), world).xyz);
    float4 clipPos = mul(wPos, viewProj);

    // clipPos.w = view-space depth (linear distance from camera).
    float dist     = clipPos.w;
    float distFade = 1.0 - saturate((dist - outlineFadeStart) / max(outlineFadeEnd - outlineFadeStart, 0.01));

    VSOut o;

    // Beyond fadeEnd: collapse behind near plane → GPU clips the triangle.
    if (distFade <= 0.0)
    {
        o.sv       = float4(0, 0, -1, 0);
        o.distFade = 0;
        return o;
    }

    // Extrude along normal in clip space (pixel-consistent width × fade).
    float4 clipNrm   = mul(float4(wNrm, 0.0f), viewProj);
    float2 nrmNDC    = clipNrm.xy / max(abs(clipPos.w), 1e-5f);
    float2 scaledNrm = nrmNDC * float2((float)vpWidth, (float)vpHeight);
    float  screenLen = length(scaledNrm);

    [flatten]
    if (screenLen > 1e-4f)
    {
        // Constant screen-space width regardless of distance;
        // distFade only affects alpha (pixel shader), not extrusion.
        float pixels = asfloat(outlinePixelsBits);
        float2 ndcOffset = (scaledNrm / screenLen) / float2((float)vpWidth, (float)vpHeight)
                           * (2.0f * pixels);
        clipPos.xy += ndcOffset * clipPos.w;
    }

    o.sv       = clipPos;
    o.distFade = distFade;
    return o;
}
