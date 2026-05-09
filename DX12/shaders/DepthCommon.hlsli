#ifndef DEPTH_COMMON_HLSLI
#define DEPTH_COMMON_HLSLI

// DepthCommon.hlsli — shared depth-buffer reconstruction helpers.
//
// Used by passes that need to recover view-space or world-space position from
// the engine's hardware depth buffer:
//   - VolumetricApply.ps.hlsl   (froxel UVW)
//   - AerialPerspective.cs.hlsl (per-voxel view ray)
//   - LightingPass (already has its own ReconstructWorldPos — could be folded
//                   in here later if a refactor is wanted).
//
// All callers use REVERSED-Z: NDC z=1 at the near plane, NDC z=0 at the far
// plane. This packs float-depth precision near the far plane where 0.0 has
// the most exponent room, and the renderer swaps near/far on the projection
// matrix builders in Renderer.cpp.

// ---- Linearise NDC z → view-Z (reversed Z) ---------------------------------
// Reversed perspective projection:
//   ndc.z = (near * far) / (view.z * (far - near))  -  near / (far - near)
// Solving for view.z:
//   view.z = (near * far) / (near + ndc.z * (far - near))
//   Check: ndc=1 (near) → view.z = n*f/f = n ✓
//          ndc=0 (far)  → view.z = n*f/n = f ✓
float DepthToViewZ(float ndcZ, float nearZ, float farZ)
{
    return (nearZ * farZ) / (nearZ + ndcZ * (farZ - nearZ));
}

// ---- World-space position from screen UV + NDC z + invViewProj -------------
// ndc.x =  uv.x * 2 - 1
// ndc.y =  1 - uv.y * 2     (DX top-left UV origin → +y NDC up)
float3 WorldPosFromDepth(float2 uv, float ndcZ, float4x4 invViewProj)
{
    float2 ndcXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 hp    = mul(float4(ndcXY, ndcZ, 1.0), invViewProj);
    return hp.xyz / hp.w;
}

// ---- Build a unit world-space view ray for a screen UV (camera-relative) ---
// Useful when you need a per-pixel ray direction independent of the actual
// depth value (e.g. ray-marching from camera). Reversed-Z convention: the
// near plane lives at NDC z=1 and the far plane at NDC z=0.
float3 ViewRayFromUV(float2 uv, float4x4 invViewProj)
{
    float2 ndcXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 nearH = mul(float4(ndcXY, 1.0, 1.0), invViewProj);
    float4 farH  = mul(float4(ndcXY, 0.0, 1.0), invViewProj);
    return normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);
}

// ---- Map view-Z into a froxel slice (exponential distribution) -------------
// Matches the Hillaire / standard volumetric froxel parameterisation.
//   slice 0       → froxelNear
//   slice depth-1 → froxelFar
//
// Forward (view-Z → 0..1 UVW.z):
float ViewZToFroxelW(float viewZ, float froxelNear, float froxelFar)
{
    float t = log(viewZ / froxelNear) / log(froxelFar / froxelNear);
    return saturate(t);
}

// Inverse (slice index 0..depth → view-Z), useful in the integration shader.
float FroxelSliceToViewZ(float slice, float froxelNear, float froxelFar, uint depth)
{
    float t = slice / float(depth);
    return froxelNear * pow(froxelFar / froxelNear, t);
}

// ---- Soft-particle depth fade ----------------------------------------------
// Returns a 0..1 multiplier that fades a transparent surface to 0 as it
// approaches geometry behind it. `pixelNdcZ` is the surface's hardware depth
// (SV_Position.z), `sceneNdcZ` is the depth buffer sample at the same texel.
// `fadeRange` is how far apart (in view-space metres) the surface must be
// from the scene before it stops being faded.
//
// Reverse-Z aware: depths are converted to view-Z first so the comparison is
// in metres regardless of projection. Surfaces in front of (or coplanar with)
// the scene → 0 (fully faded out). Surfaces well behind would be invisible
// because they fail the depth test, so this case is irrelevant.
float SoftParticleFade(float pixelNdcZ, float sceneNdcZ,
                       float nearZ, float farZ, float fadeRange)
{
    float pixelView = DepthToViewZ(pixelNdcZ, nearZ, farZ);
    float sceneView = DepthToViewZ(sceneNdcZ, nearZ, farZ);
    // Positive when the scene is farther away than the surface (normal case).
    float diff = sceneView - pixelView;
    return saturate(diff / max(fadeRange, 1e-4));
}

#endif // DEPTH_COMMON_HLSLI
