// GBuffer.vs.hlsl — PVF vertex shader.
// No Input Assembler — all vertex data fetched manually via SV_VertexID.
// Outputs per-pixel velocity for TAA motion vector buffer.

#include "pvf_fetch.hlsli"

// ---- Root parameter bindings (must match PVF root signature) ---------------

// [0] ROOT_CONSTANTS  b0 space0
cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint prevPosInfo;     // for skinned: byte offset / 12 into pos buffer (0xFFFFFFFF = none)
};

// [8] ROOT_SRV  t0 space0
#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);

// [9] ROOT_SRV  t1 space0
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);

// [1] ROOT_CBV  b1 space0
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;             // JITTERED — used for SV_POSITION only
    float4x4 prevViewProj;         // previous frame, unjittered (velocity)
    float4x4 curViewProjNoJitter;  // current frame, unjittered (velocity numerator)
};

// ---- Output struct ----------------------------------------------------------

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;  // world-space position (dedicated semantic)
    float2 uv        : TEXCOORD0;   // texture coordinates
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;     // world-space tangent
    float3 wbt       : BINORMAL;    // world-space bitangent (cross(N,T)*T.w)
    float3 col       : COLOR;       // per-vertex color (rgb; white when absent)
    float2 uv1       : TEXCOORD3;   // second UV set (0,0 when absent)
    float4 curClip   : TEXCOORD1;   // current clip-space position (for velocity)
    float4 prevClip  : TEXCOORD2;   // previous clip-space position (for velocity)
};

// ---- Main ------------------------------------------------------------------

PSIn main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];

    // Resolve index (handles both uint16/uint32 index buffers and non-indexed draws).
    uint vid = FetchIndex(md, rawID);

    // Fetch per-vertex attributes.
    float3 localPos = FETCH_POS(md, vid);
    float3 localNrm = FETCH_NORMAL(md, vid);
    float4 localTan = FETCH_TANGENT4(md, vid);  // xyz = tangent, w = handedness
    float2 uv0      = FETCH_UV0(md, vid);
    float2 uv1      = FETCH_UV1(md, vid);   // second UV set (0,0 when absent)
    float4 color    = FETCH_COLOR(md, vid); // per-vertex color (white when absent)

    // Fetch per-instance world matrix (column-major on GPU → transpose on CPU).
    float4x4 world = InstanceBuffer[instanceOffset + instID].world;

    // Transform position and TBN to world space.
    float4 wPos;
    float3 wn, wt, wbt;

#if BILLBOARD
    // Billboard: strip rotation from world matrix, face camera.
    // World matrix row 3 = translation (row-vector convention).
    float3 center = float3(world[3][0], world[3][1], world[3][2]);

    // Extract scale from world matrix (length of each basis row).
    float scaleX = length(float3(world[0][0], world[0][1], world[0][2]));
    float scaleY = length(float3(world[1][0], world[1][1], world[1][2]));

    // Camera right/up from viewProj inverse approximation:
    // For row-vector VP, the camera axes are rows of the inverse view.
    // We can extract them from the VP matrix's cofactors, but a simpler approach:
    // use the VP matrix columns (transposed rows) since VP = View * Proj.
    // Actually, extract from the world matrix of a "look at camera" orientation.
    // Simplest: use the view matrix embedded in viewProj.
    // viewProj row 0 projected gives screen-right; row 1 gives screen-up.
    // For billboard, use VP^-1 columns... but we don't have invVP here.
    //
    // Practical approach: reconstruct camera axes from viewProj.
    // VP = V * P. The first 3 rows of V^-1 = camera Right/Up/Forward.
    // VP^-1 = P^-1 * V^-1. Too complex without extra data.
    //
    // Best approach: pass camera right/up via the instance matrix itself.
    // The Renderer stores billboard orientation in the instance matrix:
    // row 0 = right * scaleX, row 1 = up * scaleY, row 3 = position.
    // This is pre-computed on CPU.
    float3 camRight = normalize(float3(world[0][0], world[0][1], world[0][2]));
    float3 camUp    = normalize(float3(world[1][0], world[1][1], world[1][2]));

    wPos = float4(center + localPos.x * camRight * scaleX + localPos.y * camUp * scaleY, 1.0);
    wn   = normalize(cross(camRight, camUp));  // face normal = toward camera
    wt   = camRight;
    wbt  = camUp;
#else
    wPos = mul(float4(localPos, 1.0f), world);
    float3x3 w3 = (float3x3)world;

    wn  = normalize(mul(localNrm, w3));

    // Robust TBN. When no per-vertex tangent stream exists, FETCH_TANGENT4
    // hands back the (1,0,0,1) fallback — fine for most surfaces, but
    // catastrophic for any vertex whose normal is exactly ±X. Gram-Schmidt
    // then computes wt - dot(wt,wn)*wn which collapses to (0,0,0), and
    // normalize(0) produces NaN. The NaN propagates through the rasterizer's
    // attribute interpolation (NaN+x=NaN) and lands in PS, where the TBN
    // multiply 0*NaN=NaN nukes the entire triangle's worldNormal even with
    // a flat (0,0,1) normal map. Sponza-style ±X-aligned walls hit this
    // perfectly and end up with NaN-stamped GBuffer normals → black lighting.
    //
    // Branch is uniform across the draw (descriptor-driven), so no warp
    // divergence cost. When the stream is present we keep the original
    // mesh-tangent path so that authored tangents (normal-mapped surfaces
    // with proper UV-space tangents) win over the synthesised basis.
    [branch]
    if (md.tangent.bufferIndex == INVALID_BUFFER)
    {
        // Synthesise an orthonormal basis from N alone. Pick a reference axis
        // that's never near-parallel with N (the |wn.y|>0.99 guard switches
        // axes for floor/ceiling normals where Y would collapse).
        float3 ref = (abs(wn.y) > 0.99) ? float3(0, 0, 1) : float3(0, 1, 0);
        wt  = normalize(cross(ref, wn));
        wbt = cross(wn, wt);
    }
    else
    {
        wt = normalize(mul(localTan.xyz, w3));
        // Gram-Schmidt against N — safe here because authored tangents
        // are guaranteed non-parallel to their vertex normal.
        wt  = normalize(wt - dot(wt, wn) * wn);
        wbt = cross(wn, wt) * localTan.w;
    }
#endif

    PSIn o;
    o.sv       = mul(wPos, viewProj);
    o.worldPos = wPos.xyz;
    o.uv       = uv0;
    o.wn       = wn;
    o.wt       = wt;
    o.wbt      = wbt;
    o.col      = color.rgb;
    o.uv1      = uv1;

    // Velocity: current and previous clip-space positions, BOTH UNJITTERED.
    // o.sv (above) is jittered so rasterization hits the sub-pixel sample
    // pattern; for the motion vector we must subtract the jitter out or
    // TAA reads "static camera = full-pixel NDC motion every frame".
    o.curClip = mul(wPos, curViewProjNoJitter);

    // Prev-frame clip pos for velocity. Both skinned and static paths multiply
    // by InstanceBuffer[].prevWorld so the entity's root motion between frames
    // is accounted for — without this, TAA reprojection for moving entities
    // (or skinned chars whose skinning output is in mesh-local space) lands
    // at the wrong history pixel, leaving motion smear that's especially
    // visible against sharp post-TAA overlays such as the outline pass.
#if BILLBOARD
    // Billboard wPos is already camera-oriented in world space; prev wPos
    // would require prev camera axes which we don't track. Camera-motion
    // velocity via current wPos × prevViewProj is the existing fallback.
    o.prevClip = mul(wPos, prevViewProj);
#else
    float4x4 prevWorld = InstanceBuffer[instanceOffset + instID].prevWorld;
    if (prevPosInfo != 0xFFFFFFFFu)
    {
        // Skinned: prev-frame skinned position in MESH-LOCAL space.
        // prevPosInfo = outPrevPosByteOffset / 12 = base element index.
        float3 prevLocalPos = FetchAsFloat3(
            md.position.bufferIndex,
            (prevPosInfo + vid) * 12u,
            md.position.format);
        float4 prevWPos = mul(float4(prevLocalPos, 1.0f), prevWorld);
        o.prevClip = mul(prevWPos, prevViewProj);
    }
    else
    {
        // Static: rest-pose local position × prev world × prev VP.
        float4 prevWPos = mul(float4(localPos, 1.0f), prevWorld);
        o.prevClip = mul(prevWPos, prevViewProj);
    }
#endif

    return o;
}
