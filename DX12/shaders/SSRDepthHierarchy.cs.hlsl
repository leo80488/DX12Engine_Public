// SSRDepthHierarchy.cs.hlsl — 2-channel depth pyramid for SSR Hi-Z traversal.
//
// Port of Wicked Engine's ssr_depthHierarchyCS.hlsl (and the underlying
// FidelityFX-SSSR approach). Stores per-texel (maxDepth, minDepth) so the
// Hi-Z ray march can pick the right "tile back wall" for its direction and
// skip whole tiles at coarse mips.
//
// Reverse-Z convention (this engine):
//   z_ndc = 1 at near plane, 0 at far plane.
//   So .r = max depth = NEAREST surface to camera in tile.
//      .g = min depth = FARTHEST surface from camera in tile.
//   Wicked's raytrace shader was already reverse-Z ("Larger z means closer")
//   — it samples .r for the standard march, matching our convention directly.
//
// Two entry points share the CB layout:
//   CSMain_Mip0   : depth SRV (t0 space2)       → u0 space2 (float2 mip 0)
//   CSMain_Reduce : prev mip UAV (u0 space2)    → u1 space2 (next mip)
//
// Root sig: shared compute layout (see GraphicsDX12::CreateComputeRootSignature).
//   [0] CBV  b0 space2  — HierCB
//   [1] SRV  t0 space2  — mip0 path: hardware depth
//   [4] UAV  u0 space2  — mip0 path: dst; reduce path: src (prev mip)
//   [5] UAV  u1 space2  — reduce path: dst (curr mip)

cbuffer HierCB : register(b0, space2)
{
    uint srcWidth;   // previous mip dimensions (for reduce clamp)
    uint srcHeight;
    uint dstWidth;   // this mip dimensions (dispatch range)
    uint dstHeight;
};

Texture2D<float>     g_Depth   : register(t0, space2);
// u0 serves both paths:
//   CSMain_Mip0:   writes (maxDepth, minDepth) into mip 0
//   CSMain_Reduce: reads prev mip and writes into… no, writes go to u1 below
RWTexture2D<float2>  g_Mip     : register(u0, space2);
RWTexture2D<float2>  g_DstMipN : register(u1, space2);  // reduce path destination

// Mip 0: copy depth texel-by-texel, set .r=.g=depth. (For 1:1 mip 0 the
// max and min collapse to the same value — the pyramid picks up variance
// from mip 1 onward.)
[numthreads(8, 8, 1)]
void CSMain_Mip0(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dstWidth || dtid.y >= dstHeight) return;

    float d = g_Depth.Load(int3(dtid.xy, 0));
    g_Mip[dtid.xy] = float2(d, d);
}

// Mip N>0: 2×2 reduce from previous mip. Max of .r across block, min of .g.
// Uses UAV-as-load so the whole pyramid stays in UNORDERED_ACCESS across the
// chain of dispatches (UAV barriers serialize writes between mips).
[numthreads(8, 8, 1)]
void CSMain_Reduce(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dstWidth || dtid.y >= dstHeight) return;

    // Clamp 2×2 sample coords to valid prev-mip range so odd-dimension reductions
    // don't drag tile_min down to 0 (far plane) via out-of-bounds loads. The
    // clamp replicates the edge texel into the missing neighbour, which is the
    // conservative choice for both max and min.
    uint2 s0 = uint2(min(dtid.x * 2u,       srcWidth  - 1u), min(dtid.y * 2u,       srcHeight - 1u));
    uint2 s1 = uint2(min(dtid.x * 2u + 1u,  srcWidth  - 1u), min(dtid.y * 2u,       srcHeight - 1u));
    uint2 s2 = uint2(min(dtid.x * 2u,       srcWidth  - 1u), min(dtid.y * 2u + 1u,  srcHeight - 1u));
    uint2 s3 = uint2(min(dtid.x * 2u + 1u,  srcWidth  - 1u), min(dtid.y * 2u + 1u,  srcHeight - 1u));

    float2 d00 = g_Mip[s0];
    float2 d10 = g_Mip[s1];
    float2 d01 = g_Mip[s2];
    float2 d11 = g_Mip[s3];

    float dMax = max(max(d00.r, d10.r), max(d01.r, d11.r));  // .r = nearest
    float dMin = min(min(d00.g, d10.g), min(d01.g, d11.g));  // .g = farthest

    g_DstMipN[dtid.xy] = float2(dMax, dMin);
}
