// FroxelTemporal.cs.hlsl
// -----------------------------------------------------------------------------
// Pass 4.5 — temporal reprojection of the just-scattered froxel grid against
// the previous frame's history. Smooths flicker from low-sample-count
// scattering and lets us amortise quality across frames.
//
//   in : ScatteringSRV (this frame)   — front-to-back integrated radiance/T
//        HistorySRV    (previous frame)
//   out: HistoryUAV    (next frame)   — alpha-blended with neighbourhood clamp
//
// The HistoryBufferComponent is double-buffered (ping-pong) on the C++ side
// so we never read/write the same resource in one dispatch.
// -----------------------------------------------------------------------------

#include "FroxelCommon.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture3D<float4>   ScatteringSRV : register(t0, space2);
Texture3D<float4>   HistorySRV    : register(t1, space2);
RWTexture3D<float4> HistoryUAV    : register(u0, space2);
SamplerState        LinClamp      : register(s0, space2);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH || DTid.z >= P.froxelD)
        return;

    float4 current = ScatteringSRV[DTid];

    // ---- Reproject to previous frame ---------------------------------------
    // Recover this voxel's world position then push through the previous
    // frame's view-projection to find where it would have been in the prior
    // history grid. Z must go through the SAME log mapping that the froxel
    // grid uses — doing linear Z here is the classic "dark stripe under
    // camera rotation" bug, because linear-Z reprojection pulls history from
    // the wrong slice of the log-distributed grid.
    float3 worldPos = FroxelToWorld(DTid, P);
    float4 prevClip = mul(float4(worldPos, 1.0), P.prevViewProj);
    if (prevClip.w <= 0.0)
    {
        HistoryUAV[DTid] = current;
        return;
    }
    float2 prevUV = float2( prevClip.x / prevClip.w * 0.5 + 0.5,
                           -prevClip.y / prevClip.w * 0.5 + 0.5);

    // View-space Z → log-distributed slice index → [0,1] UVW.z.
    float prevSliceF = ViewDepthToFroxelSlice(
        prevClip.w, P.froxelNear, P.froxelFar, P.froxelD);
    float  prevW   = prevSliceF / float(P.froxelD);
    float3 prevUVW = float3(prevUV, prevW);

    // Out-of-bounds (camera moved sideways, voxel exited frustum) → no history.
    if (any(prevUVW < 0.0) || any(prevUVW > 1.0))
    {
        HistoryUAV[DTid] = current;
        return;
    }

    // ---- Depth disocclusion -------------------------------------------------
    // Only reject history for a genuinely large Z jump (> ~8 slices). Camera
    // rotations, zoom, and cascade crossings produce several slices of drift
    // that we *want* to smooth across. 8 slices out of 128 is ~6 % of the
    // froxel depth range — catches real disocclusion (object stepping in
    // front) but forgives normal motion.
    float currSliceIdx = float(DTid.z) + 0.5;
    if (abs(prevSliceF - currSliceIdx) > 8.0)
    {
        HistoryUAV[DTid] = current;
        return;
    }

    float4 history = HistorySRV.SampleLevel(LinClamp, prevUVW, 0);

    // ---- Neighbourhood clamp (anti-ghosting) -------------------------------
    // Strict 3×3×3 = 27-neighbour min/max bbox around the current voxel. No
    // ±15 % expansion — letting history drift slightly outside the local
    // range is what fossilises the "dark stripe" artifact when a reprojection
    // is even a fraction of a slice off. Tight clamp trades a touch of
    // smoothing on gradients for reliable removal of ghosting.
    float4 nMin = current, nMax = current;
    int3 dim = int3(P.froxelW, P.froxelH, P.froxelD) - 1;
    [unroll] for (int dz = -1; dz <= 1; ++dz)
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0 && dz == 0) continue;
        int3   nc = clamp(int3(DTid) + int3(dx, dy, dz), int3(0,0,0), dim);
        float4 n  = ScatteringSRV[nc];
        nMin = min(nMin, n);
        nMax = max(nMax, n);
    }
    history = clamp(history, nMin, nMax);

    // ---- Blend -------------------------------------------------------------
    // temporalAlpha small (0.05) → heavily favour history → very smooth but
    // slow to react. Larger (0.5) → more responsive but more flicker.
    float4 result = lerp(history, current, P.temporalAlpha);
    HistoryUAV[DTid] = result;
}
