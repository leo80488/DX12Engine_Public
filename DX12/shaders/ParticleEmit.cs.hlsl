// ParticleEmit.cs.hlsl — append new particles into the global pool, with
// shape-driven initial position + velocity sampling.
//
// Dispatch: one call per emitter; threadGroups = ceil(spawnCount/64).
// Root signature (space2 + space0 + space1):
//   b0 space2  — ParticleEmitter CBV (this emitter's record)
//   u0 space2  — RWStructuredBuffer<Particle> gPool
//   (root slot 10) t1 space0 — StructuredBuffer<MeshDescriptor>  gMeshDescs
//   (root slot 11) t0 space1 — bindless ByteAddressBuffer g_Buffers[] (VB/IB)
//
// Mesh-shape sampling reads g_Buffers[meshDesc.position.bufferIndex] and
// g_Buffers[meshDesc.indexBufferIndex] via the bindless table.

#include "Particle.hlsli"

cbuffer EmitterCB : register(b0, space2)
{
    ParticleEmitter emitter;
};

RWStructuredBuffer<Particle> gPool : register(u0, space2);

// ---- MeshDescriptor (must match RHI::MeshDescriptor, 96 bytes) -------------
struct StreamDescriptor
{
    uint bufferIndex;
    uint byteOffset;
    uint byteStride;
    uint format;
};
struct MeshDescriptor
{
    StreamDescriptor position;
    StreamDescriptor normal;
    StreamDescriptor tangent;
    StreamDescriptor uv0;
    StreamDescriptor uv1;
    StreamDescriptor color;   // must mirror RHI::MeshDescriptor / pvf_fetch.hlsli (112 B)
    uint indexBufferIndex;
    uint indexByteOffset;
    uint indexFormat;    // 0 = uint16, 1 = uint32
    uint vertexCount;
};

// Bound at compute root slot 10 / 11 (same as graphics bindless PVF).
StructuredBuffer<MeshDescriptor>  gMeshDescs : register(t1, space0);
ByteAddressBuffer                 g_Buffers[] : register(t0, space1);

// ---- Helpers ---------------------------------------------------------------
float3 SamplePoint(inout uint rng)
{
    // Zero offset; velocity is already sampled in CSMain from velocityMin..Max.
    return float3(0, 0, 0);
}

float3 SampleSphere(inout uint rng, float radius, bool onShell)
{
    // Uniform on the sphere surface, then optionally scale for volume.
    float u = RandFromSeed(rng);
    float v = RandFromSeed(rng);
    float theta = 2.0 * 3.14159265 * u;
    float phi   = acos(2.0 * v - 1.0);
    float3 dir = float3(sin(phi) * cos(theta),
                        sin(phi) * sin(theta),
                        cos(phi));
    float r = onShell ? radius : radius * pow(RandFromSeed(rng), 1.0 / 3.0);
    return dir * r;
}

float3 SampleConeVelocity(inout uint rng, float3 dir, float halfAngle, float speed)
{
    // Pick a random direction within a cone around `dir`.
    float cosMax = cos(halfAngle);
    float u = RandFromSeed(rng);
    float v = RandFromSeed(rng);
    float cosT = lerp(cosMax, 1.0, u);
    float sinT = sqrt(1.0 - cosT * cosT);
    float phi  = 2.0 * 3.14159265 * v;
    // Cone-space direction: (sinT*cos(phi), sinT*sin(phi), cosT)
    float3 local = float3(sinT * cos(phi), sinT * sin(phi), cosT);
    // Build orthonormal basis around `dir`.
    float3 up = abs(dir.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 right = normalize(cross(up, dir));
    up = cross(dir, right);
    float3 world = right * local.x + up * local.y + dir * local.z;
    return normalize(world) * speed;
}

float3 SampleBox(inout uint rng, float3 halfExtents)
{
    float3 r = RandVec3(rng) * 2.0 - 1.0;  // [-1, 1]^3
    return r * halfExtents;
}

float3 SampleCircle(inout uint rng, float radius, float3 normal)
{
    // Uniform disk sample in local plane (xy), then rotate to face `normal`.
    float u = RandFromSeed(rng);
    float v = RandFromSeed(rng);
    float r = sqrt(u) * radius;
    float a = 2.0 * 3.14159265 * v;
    float3 local = float3(cos(a) * r, sin(a) * r, 0);
    // Build basis around `normal`.
    float3 up = abs(normal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 right = normalize(cross(up, normal));
    up = cross(normal, right);
    return right * local.x + up * local.y;
}

// Load a float3 vertex position via the bindless table.
float3 LoadMeshVertex(uint bufferIdx, uint byteOffsetBase, uint byteStride, uint vertexIndex)
{
    const uint byteOffset = byteOffsetBase + vertexIndex * byteStride;
    return asfloat(g_Buffers[bufferIdx].Load3(byteOffset));
}

// Load one index (uint16 or uint32) from the bindless table.
uint LoadMeshIndex(uint bufferIdx, uint byteOffsetBase, uint indexFormat, uint indexIndex)
{
    if (indexFormat == 0u)
    {
        // uint16 — two per 4 bytes.
        const uint byteOff = byteOffsetBase + indexIndex * 2;
        const uint aligned = byteOff & ~3u;
        const uint word    = g_Buffers[bufferIdx].Load(aligned);
        return (byteOff & 2u) ? (word >> 16) : (word & 0xFFFFu);
    }
    else
    {
        return g_Buffers[bufferIdx].Load(byteOffsetBase + indexIndex * 4);
    }
}

float3 SampleMesh(inout uint rng, out float3 outVelocityDir)
{
    outVelocityDir = float3(0, 1, 0);
    if (emitter.meshDescSlot == 0xFFFFFFFFu || emitter.meshIndexCount < 3)
        return float3(0, 0, 0);

    MeshDescriptor md = gMeshDescs[emitter.meshDescSlot];
    const uint triCount = emitter.meshIndexCount / 3u;
    if (triCount == 0) return float3(0, 0, 0);

    // Random triangle.
    const uint triIdx = (uint)(RandFromSeed(rng) * float(triCount)) % triCount;

    const uint i0 = LoadMeshIndex(md.indexBufferIndex, md.indexByteOffset, md.indexFormat, triIdx * 3 + 0);
    const uint i1 = LoadMeshIndex(md.indexBufferIndex, md.indexByteOffset, md.indexFormat, triIdx * 3 + 1);
    const uint i2 = LoadMeshIndex(md.indexBufferIndex, md.indexByteOffset, md.indexFormat, triIdx * 3 + 2);

    const float3 v0 = LoadMeshVertex(md.position.bufferIndex, md.position.byteOffset, md.position.byteStride, i0);
    const float3 v1 = LoadMeshVertex(md.position.bufferIndex, md.position.byteOffset, md.position.byteStride, i1);
    const float3 v2 = LoadMeshVertex(md.position.bufferIndex, md.position.byteOffset, md.position.byteStride, i2);

    // Random barycentric.
    float r1 = RandFromSeed(rng);
    float r2 = RandFromSeed(rng);
    if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
    const float r3 = 1.0 - r1 - r2;
    const float3 localPos = v0 * r1 + v1 * r2 + v2 * r3;

    // Transform by mesh world matrix (stored transposed from HLSL's viewpoint
    // when CollectEmitters copies it — keep consistent: write as-is from CPU
    // row-major; HLSL row-vector mul(pos, matrix) expects cols in CPU rows;
    // we pass un-transposed, so use mul(matrix, pos) column-vector form).
    const float4 world = mul(emitter.meshWorldMatrix, float4(localPos, 1.0));

    // Triangle normal → velocity direction (explode outward).
    const float3 e1 = v1 - v0;
    const float3 e2 = v2 - v0;
    outVelocityDir = normalize(cross(e1, e2) + 1e-6);
    outVelocityDir = normalize(mul((float3x3)emitter.meshWorldMatrix, outVelocityDir) + 1e-6);

    return world.xyz;
}

// ---- Main ------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint spawnIdx = id.x;
    if (spawnIdx >= emitter.spawnCount) return;

    uint capacity, _stride;
    gPool.GetDimensions(capacity, _stride);
    uint dst = (emitter.writeCursor + spawnIdx) % capacity;

    uint rng = emitter.randomSeed ^ (0x9E3779B9u + dst);

    // Pick a velocity sample inside velocityMin..velocityMax for every shape
    // except Cone/Mesh which override with direction×speed logic.
    float3 velocity = lerp(emitter.velocityMin, emitter.velocityMax, RandVec3(rng));

    // Sample position offset + (optionally) velocity based on shape.
    float3 posOffset = float3(0, 0, 0);
    switch (emitter.shapeType)
    {
    case PARTICLE_SHAPE_POINT:
    {
        posOffset = SamplePoint(rng);
        break;
    }
    case PARTICLE_SHAPE_SPHERE:
    {
        const float  radius   = emitter.shapeParam0.x;
        const bool   onShell  = emitter.shapeParam0.y > 0.5;
        posOffset = SampleSphere(rng, radius, onShell);
        break;
    }
    case PARTICLE_SHAPE_CONE:
    {
        const float halfAngle = emitter.shapeParam0.y;
        const float3 dir      = normalize(emitter.shapeParam1.xyz + 1e-6);
        const float length_   = emitter.shapeParam1.w;
        posOffset = dir * length_;
        // Override velocity: cone-shaped burst along `dir` with |v| from
        // velocityMin..velocityMax (use max components' magnitude as speed).
        const float speed = length(lerp(emitter.velocityMin,
                                         emitter.velocityMax,
                                         RandVec3(rng)));
        velocity = SampleConeVelocity(rng, dir, halfAngle, speed);
        break;
    }
    case PARTICLE_SHAPE_BOX:
    {
        posOffset = SampleBox(rng, emitter.shapeParam0.xyz);
        break;
    }
    case PARTICLE_SHAPE_CIRCLE:
    {
        const float  radius = emitter.shapeParam0.x;
        const float3 normal = normalize(emitter.shapeParam1.xyz + 1e-6);
        posOffset = SampleCircle(rng, radius, normal);
        break;
    }
    case PARTICLE_SHAPE_MESH:
    {
        // Mesh shape returns an ABSOLUTE world position (already transformed),
        // so overwrite `pos` below instead of offsetting from emitter.position.
        float3 velDir;
        const float3 meshPos = SampleMesh(rng, velDir);
        const float speed = length(lerp(emitter.velocityMin,
                                         emitter.velocityMax,
                                         RandVec3(rng)));
        velocity = velDir * speed;
        Particle pm = (Particle)0;
        pm.position           = meshPos;
        pm.lifetime           = emitter.startLifetime;
        pm.maxLifetime        = emitter.startLifetime;
        pm.velocity           = velocity;
        pm.startColor         = emitter.startColor;
        pm.endColor           = emitter.endColor;
        pm.startSize          = emitter.startSize;
        pm.textureBindlessIdx = emitter.textureBindlessIdx;
        pm.visualMode         = emitter.visualMode;
        gPool[dst] = pm;
        return;
    }
    }

    Particle p          = (Particle)0;
    p.position          = emitter.position + posOffset;
    p.lifetime          = emitter.startLifetime;
    p.maxLifetime       = emitter.startLifetime;
    p.velocity          = velocity;
    p.startColor        = emitter.startColor;
    p.endColor          = emitter.endColor;
    p.startSize         = emitter.startSize;
    p.textureBindlessIdx= emitter.textureBindlessIdx;
    p.visualMode        = emitter.visualMode;
    gPool[dst] = p;
}
