// HiZReduce.cs.hlsl — generate Hi-Z mip N from mip N-1.
//
// Uses UAV-as-load for the source mip so the whole Hi-Z texture can stay in
// UNORDERED_ACCESS state across every reduce dispatch (UAV barriers between
// dispatches serialize writes). That avoids per-subresource SR↔UAV
// transitions, which the engine's per-texture state tracker can't express
// without desync. Pair with HiZGenerate_CS which produces mip 0 from the
// depth SRV.
//
// Root sig (shared compute layout):
//   [0]  CBV b0 space2  — HiZCB (dst dimensions in srcWidth/Height reused
//                         as dstWidth/Height by the caller)
//   [4]  UAV u0 space2  — source mip (prev level, read-only access)
//   [5]  UAV u1 space2  — destination mip (this level, write)

cbuffer HiZCB : register(b0, space2)
{
    uint srcWidth;   // = current dstW (kept name for compat with HiZGenerate CB)
    uint srcHeight;  // = current dstH
    uint dstWidth;   // unused here
    uint dstHeight;  // unused here
};

RWTexture2D<float> g_SrcMip : register(u0, space2);
RWTexture2D<float> g_DstMip : register(u1, space2);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= srcWidth || dtid.y >= srcHeight) return;

    // 2×2 gather from the previous mip.
    uint2 s  = dtid.xy * 2;
    float d0 = g_SrcMip[s + uint2(0, 0)];
    float d1 = g_SrcMip[s + uint2(1, 0)];
    float d2 = g_SrcMip[s + uint2(0, 1)];
    float d3 = g_SrcMip[s + uint2(1, 1)];

    // Reverse-Z: near=1 far=0, min = farthest occluder in the tile.
    g_DstMip[dtid.xy] = min(min(d0, d1), min(d2, d3));
}
