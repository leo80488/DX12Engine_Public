// ParticleUpdate.cs.hlsl — advance all particles in the global pool.
//
// One thread per pool slot. Dead particles (lifetime <= 0) are skipped;
// the render VS also filters them out via degenerate quads.
//
// MVP simplification: a single global gravity lives in ParticleSystemCB
// (sampled from the FIRST emitter on the CPU side). Mixed-direction
// gravity across emitters is not supported yet — revisit if a use case
// appears (store per-particle gravity in ParticleGPU / emitter index +
// read from emitter SRV).
//
// CAUTION: the shader cache only keys off THIS file's mtime — it does
// NOT invalidate when Particle.hlsli changes. Any edit to the shared
// struct layout must touch every dependent .hlsl to force a recompile.
// Deleting shader_cache/ is the sledgehammer option.
//
// Root signature (space2):
//   b0 → ParticleSystemParams (dt, particleCount, frameIndex, gravity)
//   u0 → DESC_TABLE → gPool UAV

#include "Particle.hlsli"

cbuffer SystemCB : register(b0, space2)
{
    ParticleSystemParams gParams;
};

RWStructuredBuffer<Particle> gPool : register(u0, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint idx = id.x;
    if (idx >= gParams.particleCount) return;

    Particle p = gPool[idx];
    if (p.lifetime <= 0.0) return;

    p.velocity += gParams.globalGravity * gParams.deltaTime;
    p.position += p.velocity            * gParams.deltaTime;
    p.lifetime -= gParams.deltaTime;

    gPool[idx] = p;
}
