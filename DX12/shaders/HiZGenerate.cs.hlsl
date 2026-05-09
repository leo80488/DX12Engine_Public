// HiZGenerate.cs.hlsl — Generate one mip level of the Hierarchical-Z buffer.
//
// Each thread reads a 2x2 block from the source mip and writes the minimum
// depth to the destination mip. Dispatch once per mip level (log2 passes).
//
// Root signature (reuses compute space2):
//   [0] CBV  b0 space2  — HiZCB (source dimensions)
//   [1] SRV  t0 space2  — source mip (Texture2D<float> or prev HiZ mip)
//   [4] UAV  u0 space2  — destination mip (RWTexture2D<float>)

cbuffer HiZCB : register(b0, space2)
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
};

Texture2D<float>   g_SrcMip : register(t0, space2);
RWTexture2D<float> g_DstMip : register(u0, space2);

SamplerState g_PointClamp : register(s0, space2);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dstWidth || dtid.y >= dstHeight) return;

    // Source texel coordinates (2x2 block)
    uint2 src = dtid.xy * 2;

    // Sample 4 texels from source mip using Load (point sample)
    float d00 = g_SrcMip.Load(int3(src + uint2(0, 0), 0));
    float d10 = g_SrcMip.Load(int3(src + uint2(1, 0), 0));
    float d01 = g_SrcMip.Load(int3(src + uint2(0, 1), 0));
    float d11 = g_SrcMip.Load(int3(src + uint2(1, 1), 0));

    // Reversed-Z: near=1, far=0. MIN over the 2x2 block yields the smallest
    // z = the FARTHEST occluder in the tile, which is what Hi-Z needs for
    // conservative occlusion (AABB visible if nearest corner >= tile farthest).
    float minDepth = min(min(d00, d10), min(d01, d11));

    g_DstMip[dtid.xy] = minDepth;
}
