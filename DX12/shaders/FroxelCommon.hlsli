#ifndef FROXEL_COMMON_HLSLI
#define FROXEL_COMMON_HLSLI

// Shared header for the volumetric-fog froxel pipeline.
//   FroxelDensity.cs.hlsl     — fills 3D density texture
//   FroxelLightInject.cs.hlsl — multiplies density by per-light scattering
//   FroxelScatter.cs.hlsl     — front-to-back integration along z
//   VolumetricApply.ps.hlsl   — composite onto HDR scene
//
// Coordinate convention:
//   Froxel (x, y, z) → screen UV (xy) + exponential view-space depth slice (z).
//   z=0 is near, z=Depth−1 is far. Depth uses log(zFar/zNear) distribution.

#ifndef FROXEL_PI
#define FROXEL_PI 3.14159265358979
#endif

// All floats / dims supplied by FroxelCB which the consumer must declare with
// matching layout (see VolumetricFogPass.cpp — FroxelConstants).
struct FroxelParams
{
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 prevViewProj;

    float3 cameraPos;          float nearPlane;
    float farPlane;            float froxelNear;
    float froxelFar;           float temporalAlpha;

    uint   frameIndex;
    uint   froxelW;
    uint   froxelH;
    uint   froxelD;

    float fogDensity;
    float fogScattering;
    float fogAbsorption;
    float anisotropy;          // Henyey-Greenstein g

    float heightFogStart;
    float heightFogFalloff;
    float ambientContribution;
    float _pad0;

    float3 sunDir;             float sunStrength;     // sunDir is FROM ground (lightDir convention)
    float3 sunColor;           float _pad1;
    float3 ambientColor;       float _pad2;

    // CSM (3 cascade matrices + per-cascade view-Z splits + sampling params).
    // Packed into the same CB as the main params so we only need one b0 slot
    // for compute (the engine's compute root sig only exposes b0 space2).
    float4x4 shadowMatrix0;
    float4x4 shadowMatrix1;
    float4x4 shadowMatrix2;
    float4   cascadeSplits;     // .xyz = far view-Z of cascade 0/1/2
    float4   shadowParams;      // x=texelSize, y=blendRange, z=bias, w=strength
    float3   cameraForward;    float _pad3;

    // Voxel occupancy grid bounds (written by SceneVoxelPass,
    // sampled by FroxelLightInject for volumetric-light occlusion).
    float3   voxelGridMin;     float _padVG0;
    float3   voxelGridExtent;  uint  voxelGridDim; // extent = max - min
};

// ---------------------------------------------------------------------------
// Slice index ↔ view-space depth (exponential distribution).
//   z=0    → froxelNear
//   z=D−1  → froxelFar
// ---------------------------------------------------------------------------
float FroxelSliceToViewDepth(float slice, float zNear, float zFar, uint depth)
{
    float t = slice / float(depth);
    return zNear * pow(zFar / zNear, t);
}

float ViewDepthToFroxelSlice(float depth, float zNear, float zFar, uint depthSlices)
{
    return log(depth / zNear) / log(zFar / zNear) * float(depthSlices);
}

// ---------------------------------------------------------------------------
// Per-frame sub-voxel jitter (Halton-ish 8-tap, indexed by FrameIndex & 7).
// Used to anti-alias the cone edge / shadow boundary by sampling a slightly
// different position within the voxel each frame; combined with temporal
// reprojection this converges to smooth gradients instead of hard rings.
// ---------------------------------------------------------------------------
float3 GetFroxelJitter(uint frameIdx)
{
    // 16-tap Halton-(2,3,5) low-discrepancy sequence in the [-0.5, +0.5]
    // cube — i.e. a full-voxel jitter range. Now that the temporal pass uses
    // correct row-vector transposed matrices (the previous ±0.25 was chosen
    // to work around broken reprojection that made history always a fraction
    // of a voxel off), we can spread samples across the entire voxel volume.
    // Over the 16-frame cycle every voxel's time-averaged sample becomes an
    // integral over its full volume — effectively box-filtered per voxel,
    // which is exactly what kills cone-edge aliasing.
    static const float3 kJitter[16] = {
        float3( 0.0000,  0.0000,  0.0000),
        float3(-0.2500,  0.3333, -0.2000),
        float3( 0.2500, -0.3333,  0.2000),
        float3(-0.3750, -0.1111,  0.4000),
        float3( 0.3750,  0.1111, -0.4000),
        float3(-0.1250,  0.4444,  0.1000),
        float3( 0.1250, -0.4444, -0.1000),
        float3(-0.4375, -0.2222,  0.3000),
        float3( 0.4375,  0.2222, -0.3000),
        float3(-0.1875,  0.1111, -0.4400),
        float3( 0.1875, -0.1111,  0.4400),
        float3(-0.3125,  0.4999, -0.1600),
        float3( 0.3125, -0.4999,  0.1600),
        float3(-0.0625, -0.3888,  0.3600),
        float3( 0.0625,  0.3888, -0.3600),
        float3(-0.4999,  0.1666,  0.4800)
    };
    return kJitter[frameIdx & 15u];
}

// ---------------------------------------------------------------------------
// Froxel index → world-space position. `jitter` is in voxel-space ([-0.5,
// +0.5] per axis) and lets the caller offset the sample within the voxel for
// temporal supersampling. Use FroxelToWorld(id, p) for the un-jittered centre.
// ---------------------------------------------------------------------------
float3 FroxelToWorldJittered(uint3 id, FroxelParams p, float3 jitter)
{
    float2 uv     = (float2(id.xy) + 0.5 + jitter.xy) / float2(p.froxelW, p.froxelH);
    // NDC: x [-1, +1], y [+1, -1] (DX top-left origin), z [0, 1]
    float2 ndc    = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float  viewZ  = FroxelSliceToViewDepth(float(id.z) + 0.5 + jitter.z,
                                            p.froxelNear, p.froxelFar, p.froxelD);

    // Reversed-Z: near plane at NDC z=1, far plane at NDC z=0.
    float4 nearH = mul(float4(ndc, 1.0, 1.0), p.invViewProj);
    float4 farH  = mul(float4(ndc, 0.0, 1.0), p.invViewProj);
    float3 nearW = nearH.xyz / nearH.w;
    float3 farW  = farH.xyz  / farH.w;

    float3 dir   = normalize(farW - nearW);
    return p.cameraPos + dir * viewZ;
}

float3 FroxelToWorld(uint3 id, FroxelParams p)
{
    return FroxelToWorldJittered(id, p, float3(0, 0, 0));
}

// ---------------------------------------------------------------------------
// World-space position → froxel UVW (for trilinear sampling at apply time).
// ---------------------------------------------------------------------------
float3 WorldToFroxelUVW(float3 worldPos, FroxelParams p)
{
    float4 clip   = mul(float4(worldPos, 1.0), p.viewProj);
    float  viewZ  = clip.w;                     // perspective W = view-space depth
    if (viewZ <= 0.0) return float3(0, 0, 0);
    float3 ndc    = clip.xyz / clip.w;
    float2 uv     = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    float  slice  = ViewDepthToFroxelSlice(viewZ, p.froxelNear, p.froxelFar, p.froxelD);
    return float3(uv, slice / float(p.froxelD));
}

// ---------------------------------------------------------------------------
// Henyey-Greenstein phase function — controls forward / backward scattering.
//   g →  1: strongly forward (Mie)
//   g →  0: isotropic
//   g → -1: strongly backward
// ---------------------------------------------------------------------------
float PhaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    float den = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * FROXEL_PI * pow(max(den, 1e-3), 1.5));
}

#endif // FROXEL_COMMON_HLSLI
