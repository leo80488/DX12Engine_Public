// Skin.cs.hlsl — GPU skinning compute shader (Dual Quaternion Skinning)
//
// Root signature (compute, must match CreateComputeRootSignature in GraphicsDX12.cpp):
//   [0]  ROOT_CBV      b0 space2  — SkinJobDesc
//   [1]  DESC_TABLE    t0 space2  — rest position buffer (StructuredBuffer<float3>)
//   [2]  DESC_TABLE    t1 space2  — rest normal buffer   (StructuredBuffer<float3>)
//   [3]  DESC_TABLE    t2 space2  — blend data buffer    (ByteAddressBuffer, 24 B/vertex)
//   [7]  DESC_TABLE    t3 space2  — pose matrix buffer   (ByteAddressBuffer, float4x4[])
//   [4]  DESC_TABLE    u0 space2  — output position buffer (RWStructuredBuffer<float3>)
//   [5]  DESC_TABLE    u1 space2  — output normal buffer   (RWStructuredBuffer<float3>)
//
// Thread count: [numthreads(64,1,1)], dispatch ceil(vertexCount/64) groups.
// One thread per vertex. Early-out when tid.x >= vertexCount.
//
// Dual Quaternion Skinning (DQS) preserves volume and avoids the candy-wrapper
// collapse artifacts of Linear Blend Skinning (LBS).  Each bone's rigid transform
// is converted to a dual quaternion, blended in DQ space, then applied to the vertex.

// ---------------------------------------------------------------------------
// Per-job descriptor (uploaded as StructuredBuffer, one entry per skinned mesh)
// ---------------------------------------------------------------------------
struct SkinJobDesc
{
    uint  poseByteOffset;         // byte offset into g_Pose for this entity's bones
    uint  vertexCount;            // number of vertices
    uint  outPosByteOffset;       // byte offset into g_OutPos for this entity's output
    uint  outNrmByteOffset;       // byte offset into g_OutNrm for this entity's output
    uint  morphCount;             // number of morph targets (0 = no morphs)
    uint  morphWeightByteOffset;  // byte offset into g_MorphWeights
    uint  prevPoseByteOffset;     // byte offset into g_PrevPose (0xFFFFFFFF = none)
    uint  outPrevPosByteOffset;   // byte offset into g_OutPrevPos
};

// Per-job data — root CBV points at the current job's slice in the upload buffer.
ConstantBuffer<SkinJobDesc> g_Job : register(b0, space2);

// ---------------------------------------------------------------------------
// SRV inputs
// ---------------------------------------------------------------------------
StructuredBuffer<float3>   g_RestPos      : register(t0, space2); // rest-pose positions
StructuredBuffer<float3>   g_RestNrm      : register(t1, space2); // rest-pose normals
ByteAddressBuffer          g_Blend        : register(t2, space2); // BlendVertex (16 B/vertex)
ByteAddressBuffer          g_Pose         : register(t3, space2); // float4x4 bone matrices
StructuredBuffer<float3>   g_MorphDeltas  : register(t4, space2); // morph vertex deltas
ByteAddressBuffer          g_MorphWeights : register(t5, space2); // per-job float[128] weights

// ---------------------------------------------------------------------------
// UAV outputs
// ---------------------------------------------------------------------------
RWStructuredBuffer<float3>  g_OutPos : register(u0, space2); // skinned positions
RWStructuredBuffer<float3>  g_OutNrm : register(u1, space2); // skinned normals

// ---------------------------------------------------------------------------
// Helper: load a row-major float4x4 from g_Pose at byteOffset.
// ---------------------------------------------------------------------------
float4x4 LoadBoneMat(uint byteOff)
{
    float4 r0 = asfloat(g_Pose.Load4(byteOff +  0));
    float4 r1 = asfloat(g_Pose.Load4(byteOff + 16));
    float4 r2 = asfloat(g_Pose.Load4(byteOff + 32));
    float4 r3 = asfloat(g_Pose.Load4(byteOff + 48));
    return float4x4(r0, r1, r2, r3);
}

// =========================================================================
// Dual Quaternion helpers
//
// A dual quaternion DQ = (q_real, q_dual) where:
//   q_real = rotation quaternion (unit)
//   q_dual = 0.5 * translation_quat * q_real
//
// We store as two float4: real(x,y,z,w), dual(x,y,z,w).
// =========================================================================

struct DualQuat
{
    float4 real;  // rotation part (unit quaternion)
    float4 dual;  // translation part
};

// Quaternion multiply: a * b (Hamilton product)
float4 QMul(float4 a, float4 b)
{
    return float4(
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    );
}

// Convert a row-major row-vector 4x4 matrix to a dual quaternion.
// The matrix is assumed to be a rigid transform (rotation + translation, no scale).
// Row-vector convention: v' = v * M, translation is in row 3 (M[3][0..2]).
DualQuat MatToDQ(float4x4 m)
{
    DualQuat dq;

    // Extract rotation quaternion from the 3x3 upper-left block.
    // The matrix is row-major row-vector, so rows are basis vectors.
    float3x3 rot = float3x3(m[0].xyz, m[1].xyz, m[2].xyz);

    // Shepperd's method for robust quaternion extraction
    float tr = rot[0][0] + rot[1][1] + rot[2][2];
    float4 q;

    if (tr > 0.0f)
    {
        float s = sqrt(tr + 1.0f) * 2.0f; // s = 4*w
        q.w = 0.25f * s;
        q.x = (rot[1][2] - rot[2][1]) / s;
        q.y = (rot[2][0] - rot[0][2]) / s;
        q.z = (rot[0][1] - rot[1][0]) / s;
    }
    else if (rot[0][0] > rot[1][1] && rot[0][0] > rot[2][2])
    {
        float s = sqrt(1.0f + rot[0][0] - rot[1][1] - rot[2][2]) * 2.0f;
        q.w = (rot[1][2] - rot[2][1]) / s;
        q.x = 0.25f * s;
        q.y = (rot[1][0] + rot[0][1]) / s;
        q.z = (rot[2][0] + rot[0][2]) / s;
    }
    else if (rot[1][1] > rot[2][2])
    {
        float s = sqrt(1.0f + rot[1][1] - rot[0][0] - rot[2][2]) * 2.0f;
        q.w = (rot[2][0] - rot[0][2]) / s;
        q.x = (rot[1][0] + rot[0][1]) / s;
        q.y = 0.25f * s;
        q.z = (rot[2][1] + rot[1][2]) / s;
    }
    else
    {
        float s = sqrt(1.0f + rot[2][2] - rot[0][0] - rot[1][1]) * 2.0f;
        q.w = (rot[0][1] - rot[1][0]) / s;
        q.x = (rot[2][0] + rot[0][2]) / s;
        q.y = (rot[2][1] + rot[1][2]) / s;
        q.z = 0.25f * s;
    }

    q = normalize(q);
    dq.real = q;

    // Dual part: d = 0.5 * t_quat * q_real
    // where t_quat = (tx, ty, tz, 0) as a pure quaternion.
    // Translation is in row 3 for row-vector convention.
    float3 t = m[3].xyz;
    float4 tq = float4(t, 0.0f);
    dq.dual = QMul(tq, q) * 0.5f;

    return dq;
}

// Normalize a blended dual quaternion.
DualQuat NormalizeDQ(DualQuat dq)
{
    float len = length(dq.real);
    if (len < 1e-8f) len = 1.0f;
    float invLen = 1.0f / len;
    dq.real *= invLen;
    dq.dual *= invLen;
    return dq;
}

// Transform a position by a dual quaternion.
// p' = q * p * q* + 2 * d * q*   (simplified form)
float3 DQTransformPoint(DualQuat dq, float3 p)
{
    // Rotation: p' = q * p * q*
    float4 q = dq.real;
    float3 t_extract = 2.0f * (q.w * dq.dual.xyz - dq.dual.w * q.xyz
                                + cross(q.xyz, dq.dual.xyz));

    // Rotate position
    float3 rotated = p + 2.0f * cross(q.xyz, cross(q.xyz, p) + q.w * p);

    return rotated + t_extract;
}

// Transform a normal/direction by a dual quaternion (rotation only).
float3 DQTransformNormal(DualQuat dq, float3 n)
{
    float4 q = dq.real;
    return n + 2.0f * cross(q.xyz, cross(q.xyz, n) + q.w * n);
}

// ---------------------------------------------------------------------------
// Main compute kernel
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void SkinCS(uint3 tid : SV_DispatchThreadID)
{
    const uint v = tid.x;
    if (v >= g_Job.vertexCount) return;

    // --- Vertex index accounting for entity's output slice ---
    const uint outPosIdx = g_Job.outPosByteOffset / 12u + v;
    const uint outNrmIdx = g_Job.outNrmByteOffset / 12u + v;

    // --- Load blend data (24 bytes per vertex) ---
    // Layout: uint16 boneIndices[8] (16 bytes) + uint8 weights[8] (8 bytes)
    // Load as 6 × uint32 = 24 bytes
    const uint baseOff = v * 24u;
    const uint4 raw0 = g_Blend.Load4(baseOff);       // boneIdx[0..7] as 4 uint32s (16 bytes)
    const uint2 raw1 = g_Blend.Load2(baseOff + 16u);  // weights[0..7] as 2 uint32s (8 bytes)

    uint  bIdx[8];
    float bWgt[8];
    // Unpack uint16 bone indices (2 per uint32)
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        uint packed = (k < 2) ? ((k == 0) ? raw0.x : raw0.y) : ((k == 2) ? raw0.z : raw0.w);
        bIdx[k * 2]     = packed & 0xFFFFu;
        bIdx[k * 2 + 1] = (packed >> 16) & 0xFFFFu;
    }
    // Unpack uint8 weights (4 per uint32)
    [unroll] for (uint j = 0; j < 4; ++j)
    {
        bWgt[j]     = float((raw1.x >> (j * 8)) & 0xFFu) * (1.0f / 255.0f);
        bWgt[j + 4] = float((raw1.y >> (j * 8)) & 0xFFu) * (1.0f / 255.0f);
    }

    // --- Load rest-pose data + apply morph deltas ---
    float3 restPos = g_RestPos[v];
    float3 restNrm = g_RestNrm[v];

    // Accumulate vertex morph deltas: restPos += Σ(weight[m] * delta[m][v])
    // g_MorphDeltas layout: [morphIdx * vertexCount + vertexIdx]
    // g_MorphWeights layout: ByteAddressBuffer, float[128] per job at morphWeightByteOffset
    if (g_Job.morphCount > 0)
    {
        const uint vc   = g_Job.vertexCount;
        const uint wOff = g_Job.morphWeightByteOffset;
        [loop] for (uint m = 0; m < g_Job.morphCount; ++m)
        {
            float w = asfloat(g_MorphWeights.Load(wOff + m * 4u));
            if (w > 0.001f)
                restPos += g_MorphDeltas[m * vc + v] * w;
        }
    }

    // --- Load up to 8 bone matrices, convert to dual quaternions, blend ---
    const uint pBase = g_Job.poseByteOffset;

    // Start with first bone that has weight > 0
    DualQuat dq0 = MatToDQ(LoadBoneMat(pBase + bIdx[0] * 64u));

    DualQuat blended;
    blended.real = dq0.real * bWgt[0];
    blended.dual = dq0.dual * bWgt[0];

    // Accumulate remaining bones with antipodality fix
    [unroll] for (uint i = 1; i < 8; ++i)
    {
        if (bWgt[i] > 0.0f)
        {
            DualQuat dqi = MatToDQ(LoadBoneMat(pBase + bIdx[i] * 64u));
            // Antipodality: flip if in opposite hemisphere
            float sign = (dot(dq0.real, dqi.real) < 0.0f) ? -1.0f : 1.0f;
            blended.real += dqi.real * (bWgt[i] * sign);
            blended.dual += dqi.dual * (bWgt[i] * sign);
        }
    }

    // --- Normalize the blended dual quaternion ---
    blended = NormalizeDQ(blended);

    // --- Apply DQ skinning ---
    const float3 skinnedPos = DQTransformPoint(blended, restPos);
    const float3 skinnedNrm = normalize(DQTransformNormal(blended, restNrm));

    // --- Write output ---
    g_OutPos[outPosIdx] = skinnedPos;
    g_OutNrm[outNrmIdx] = skinnedNrm;

    // --- Previous frame skinned position (for TAA velocity) ---
    // Previous bone matrices are stored at prevPoseByteOffset in the same g_Pose buffer.
    // If available, skin the vertex with previous bones and write to a separate offset.
    if (g_Job.prevPoseByteOffset != 0xFFFFFFFFu)
    {
        const uint prevBase = g_Job.prevPoseByteOffset;

        DualQuat prevDQ0 = MatToDQ(LoadBoneMat(prevBase + bIdx[0] * 64u));
        DualQuat prevBlended;
        prevBlended.real = prevDQ0.real * bWgt[0];
        prevBlended.dual = prevDQ0.dual * bWgt[0];

        [unroll] for (uint pi = 1; pi < 8; ++pi)
        {
            if (bWgt[pi] > 0.0f)
            {
                DualQuat pdqi = MatToDQ(LoadBoneMat(prevBase + bIdx[pi] * 64u));
                float psign = (dot(prevDQ0.real, pdqi.real) < 0.0f) ? -1.0f : 1.0f;
                prevBlended.real += pdqi.real * (bWgt[pi] * psign);
                prevBlended.dual += pdqi.dual * (bWgt[pi] * psign);
            }
        }
        prevBlended = NormalizeDQ(prevBlended);

        // Write to the same g_OutPos buffer at a separate offset
        const uint outPrevIdx = g_Job.outPrevPosByteOffset / 12u + v;
        g_OutPos[outPrevIdx] = DQTransformPoint(prevBlended, restPos);
    }
}
