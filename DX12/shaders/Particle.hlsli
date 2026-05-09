// Particle.hlsli — shared particle GPU data layouts.
//
// Layout must match:
//   include/Graphics/ParticleSystem.h  (ParticleGPU / ParticleEmitterGPU / ParticleSystemCB)

#ifndef PARTICLE_HLSLI
#define PARTICLE_HLSLI

// 80 bytes. Matches ParticleGPU on CPU.
struct Particle
{
    float3 position;            // 12
    float  lifetime;            // 4 — seconds remaining; <=0 = dead
    float3 velocity;            // 12
    float  maxLifetime;         // 4 — total life (for 0..1 lerp)
    float4 startColor;          // 16
    float4 endColor;            // 16
    float  startSize;           // 4
    uint   textureBindlessIdx;  // 4 — 0xFFFFFFFFu = no texture
    uint   visualMode;          // 4 — ParticleVisualMode enum cast
    float  _pad0;               // 4
};

// 256 bytes. Matches ParticleEmitterGPU.
struct ParticleEmitter
{
    float3   position;
    float    startLifetime;
    float3   velocityMin;
    float    startSize;
    float3   velocityMax;
    uint     spawnCount;
    float4   startColor;
    float4   endColor;
    float3   gravity;
    uint     writeCursor;
    uint     randomSeed;
    uint     shapeType;          // ParticleShape enum
    uint     visualMode;         // ParticleVisualMode enum
    uint     textureBindlessIdx; // 0xFFFFFFFFu = none
    float4   shapeParam0;
    float4   shapeParam1;
    float4   shapeParam2;
    float4   shapeParam3;
    float4x4 meshWorldMatrix;
    uint     meshDescSlot;
    uint     meshIndexCount;
    uint2    _pad;
};

// Shape type constants (must match C++ ParticleShape enum).
#define PARTICLE_SHAPE_POINT   0u
#define PARTICLE_SHAPE_SPHERE  1u
#define PARTICLE_SHAPE_CONE    2u
#define PARTICLE_SHAPE_BOX     3u
#define PARTICLE_SHAPE_CIRCLE  4u
#define PARTICLE_SHAPE_MESH    5u

// Visual mode constants (must match C++ ParticleVisualMode enum).
#define PARTICLE_VISUAL_FLAT     0u
#define PARTICLE_VISUAL_FIRE     1u
#define PARTICLE_VISUAL_SMOKE    2u
#define PARTICLE_VISUAL_ELECTRIC 3u

// 32 bytes. Matches ParticleSystemCB.
struct ParticleSystemParams
{
    float  deltaTime;
    uint   particleCount;
    uint   frameIndex;
    float  _pad0;
    float3 globalGravity;
    float  _pad1;
};

// Cheap integer hash → unit float. Based on PCG; deterministic.
float RandFromSeed(inout uint seed)
{
    seed = seed * 747796405u + 2891336453u;
    uint word = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word & 0x00FFFFFFu) / float(0x01000000u);
}

float3 RandVec3(inout uint seed)
{
    return float3(RandFromSeed(seed), RandFromSeed(seed), RandFromSeed(seed));
}

#endif // PARTICLE_HLSLI
