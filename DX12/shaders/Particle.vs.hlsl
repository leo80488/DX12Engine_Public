// Particle.vs.hlsl — camera-facing billboard quad VS driven by a
// StructuredBuffer<Particle>. One instance per pool slot, 4 verts per
// instance. Dead particles (lifetime <= 0) emit a degenerate quad so the
// rasteriser skips them without an explicit culling pass.
//
// Root signature (graphics space0):
//   b2 space0 → ParticleRenderCB (cam right/up, viewProj)
//   t4 space0 → DESC_TABLE pointing at gPool (SRV of the particle buffer)
//
// Slot register mapping explained in ParticleRenderPass.cpp.

#include "Particle.hlsli"

cbuffer ParticleRenderCB : register(b2, space0)
{
    float4x4 viewProj;     // jittered (for SV_POSITION)
    float3   camRight;     // world-space camera right
    float    particleSize; // scale multiplier (default 1.0)
    float3   camUp;        // world-space camera up
    float    _pad;
};

StructuredBuffer<Particle> gPool : register(t4, space0);

struct VSOut
{
    float4 pos     : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float4 color   : COLOR0;
    nointerpolation uint texIdx  : TEXCOORD1;   // bindless texture index (0xFFFFFFFFu = none)
    nointerpolation uint visMode : TEXCOORD2;   // ParticleVisualMode enum
    float  lifeT   : TEXCOORD3;   // 0..1 age for PS visual mode ramps
};

// 4 verts per instance, arranged as a triangle strip:
//   (-1,-1) (1,-1) (-1,1) (1,1)
static const float2 kQuadOffsets[4] =
{
    float2(-1.0, -1.0),
    float2( 1.0, -1.0),
    float2(-1.0,  1.0),
    float2( 1.0,  1.0),
};

VSOut main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOut o = (VSOut)0;

    Particle p = gPool[instanceID];

    // Dead particle → degenerate quad at origin. The rasteriser discards
    // zero-area triangles without entering the PS.
    if (p.lifetime <= 0.0 || p.maxLifetime <= 0.0)
    {
        o.pos    = float4(0, 0, 0, 1);
        o.uv     = float2(0, 0);
        o.color  = float4(0, 0, 0, 0);
        o.texIdx = 0xFFFFFFFFu;
        o.visMode = 0u;
        o.lifeT  = 0.0;
        return o;
    }

    // 0..1 normalized life (0 = just spawned, 1 = about to die).
    float lifeT = saturate(1.0 - p.lifetime / p.maxLifetime);

    // Size taper: start at p.startSize, shrink to 70% at end of life so
    // particles don't pop out abruptly. `particleSize` from the CB is a
    // global multiplier (1.0 by default) the editor can use for scene-wide
    // scaling without touching every emitter.
    float size = p.startSize * particleSize * (1.0 - 0.3 * lifeT);

    // World-space billboard: expand from particle centre along camera axes.
    float2 off = kQuadOffsets[vertexID] * size;
    float3 worldPos = p.position
                    + camRight * off.x
                    + camUp    * off.y;

    o.pos     = mul(float4(worldPos, 1.0), viewProj);
    o.uv      = kQuadOffsets[vertexID] * 0.5 + 0.5;
    o.color   = lerp(p.startColor, p.endColor, lifeT);
    o.texIdx  = p.textureBindlessIdx;
    o.visMode = p.visualMode;
    o.lifeT   = lifeT;

    return o;
}

