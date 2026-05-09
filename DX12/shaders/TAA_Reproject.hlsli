#ifndef TAA_REPROJECT_HLSLI
#define TAA_REPROJECT_HLSLI

#include "TAA_Common.hlsli"

// -----------------------------------------------------------------------------
// TAA_Reproject.hlsli
//
// History reprojection helpers — velocity dilation and offscreen test.
// -----------------------------------------------------------------------------

// Karis-style depth-aware velocity dilation (UE / Karis 2014).
//
// Picks the velocity from the 3x3 neighbour with the smallest depth (i.e. the
// pixel nearest to the camera). Rationale: at a silhouette between a slow
// foreground object and a fast background (or vice-versa), longest-magnitude
// dilation will pick the BACKGROUND velocity for foreground-edge pixels —
// causing the foreground edge to ghost-trail the background's motion. The
// nearest-depth heuristic always biases toward the foreground surface, which
// is structurally correct because that is the surface contributing the
// majority of shading at the silhouette pixel.
//
// Reference: Karis 2014 "High Quality Temporal Supersampling" §4.1.
//   Replaces the older Falcor-style longest-magnitude approach.

float2 DilateVelocity(Texture2D<float2> velTex,
                       Texture2D<float>  depthTex,
                       int2              pos,
                       int2              dim)
{
    int2  closestPos   = pos;
    float closestDepth = depthTex.Load(int3(pos, 0)).r;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    [unroll]
    for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0) continue;
        int2  p = clamp(pos + int2(dx, dy), int2(0, 0), dim - 1);
        float d = depthTex.Load(int3(p, 0)).r;
        // Engine uses REVERSED-Z (near=1.0, far=0.0, GREATER_EQUAL test).
        // "Closest to camera" therefore means LARGER depth value, not smaller.
        if (d > closestDepth)
        {
            closestDepth = d;
            closestPos   = p;
        }
    }
    return velTex.Load(int3(closestPos, 0));
}

// Apply NDC-space velocity to current screen UV.
//   velocity is (curNDC - prevNDC), so prevNDC = curNDC - velocity.
float2 ReprojectUV(float2 uv, float2 velocity)
{
    float2 curNDC  = UVToNDC(uv);
    float2 prevNDC = curNDC - velocity;
    return NDCToUV(prevNDC);
}

bool IsUVOutside(float2 uv)
{
    return any(uv < 0.0) || any(uv > 1.0);
}

#endif // TAA_REPROJECT_HLSLI
