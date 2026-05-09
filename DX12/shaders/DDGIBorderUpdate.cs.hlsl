// DDGIBorderUpdate.cs.hlsl — copy interior octahedral edges into the atlas
// border so bilinear sampling at the seams stays correct.
//
// Octahedral mapping has a well-known seam: the edges of the [-1,1]² square
// map to antipodal points on the sphere. To make a hardware bilinear sample
// produce a smooth result, the +1-texel border is filled with the *opposite*
// edge's content (mirrored). This kernel runs once per (probe, border-texel)
// after Relight has updated the interiors.
//
// Two dispatches share the kernel via the same DDGI_RELIGHT_TARGET_* defines
// as DDGIRelight.cs (irradiance / depth). The output format differs but the
// addressing is identical.

#include "DDGICommon.hlsli"

#ifndef DDGI_RELIGHT_TARGET_IRRADIANCE
#define DDGI_RELIGHT_TARGET_IRRADIANCE 1
#endif
#ifndef DDGI_RELIGHT_TARGET_DEPTH
#define DDGI_RELIGHT_TARGET_DEPTH 0
#endif

ConstantBuffer<DDGIVolumeGPU>      g_Vol   : register(b0, space0);

#if DDGI_RELIGHT_TARGET_IRRADIANCE
RWTexture2D<float4>                g_Atlas : register(u0, space0);
static const uint kProbeSize = DDGI_IRRADIANCE_PROBE_SIZE;
static const uint kProbeStride = DDGI_IRRADIANCE_PROBE_STRIDE;
#else
RWTexture2D<float2>                g_Atlas : register(u0, space0);
static const uint kProbeSize = DDGI_DEPTH_PROBE_SIZE;
static const uint kProbeStride = DDGI_DEPTH_PROBE_STRIDE;
#endif

// Map a border texel (in the +/-1 ring around the interior) to its mirror
// inside the interior. The octahedral remap rule:
//   x = -1     → mirror across diagonal: (probeSize-1-y, 0) etc.
// (Implementation borrows McGuire 2017 / RTXGI sample reference.)
uint2 BorderToInterior(int x, int y, uint probeSize)
{
    int p = (int)probeSize;
    if (x < 0)              return uint2(0,                p - 1 - y);
    if (y < 0)              return uint2(p - 1 - x,        0);
    if (x >= p)             return uint2(p - 1,            p - 1 - y);
    if (y >= p)             return uint2(p - 1 - x,        p - 1);
    return uint2(x, y);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    const uint probeIdx = tid.x;
    if (probeIdx >= g_Vol.probeCountsX * g_Vol.probeCountsY * g_Vol.probeCountsZ)
        return;

    // Threads cover the (probeStride * probeStride) tile. Skip interior pixels
    // (those were written by Relight); only fill border ring.
    int x = (int)lid.x; // 0..stride-1
    int y = (int)lid.y;
    if (x >= (int)kProbeStride || y >= (int)kProbeStride) return;

    // Interior occupies [1..probeSize], borders are 0 and probeSize+1.
    int ix = x - 1; // -1, 0..probeSize-1, probeSize
    int iy = y - 1;
    bool isBorder = (ix < 0) || (iy < 0) || (ix >= (int)kProbeSize) || (iy >= (int)kProbeSize);
    if (!isBorder) return;

    uint2 srcLocal = BorderToInterior(ix, iy, kProbeSize);

    // Atlas positions for both ends of the copy.
    uint2 tile = DDGI_ProbeAtlasTile(DDGI_ProbeCoord(probeIdx, g_Vol), g_Vol, kProbeStride);
    uint2 dstPx = tile + uint2(x, y);
    uint2 srcPx = tile + uint2(1, 1) + srcLocal;

#if DDGI_RELIGHT_TARGET_IRRADIANCE
    g_Atlas[dstPx] = g_Atlas[srcPx];
#else
    g_Atlas[dstPx] = g_Atlas[srcPx];
#endif
}
