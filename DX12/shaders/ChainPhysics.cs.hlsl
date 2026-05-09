// ChainPhysics.cs.hlsl — GPU Verlet + PBD constraint solver for hair/skirt chains.
//
// Three entry points, dispatched sequentially with UAV barriers:
//   1. CSIntegrate   — Verlet integration (gravity + damping)
//   2. CSConstraint  — PBD distance constraint (one color group per dispatch)
//   3. CSWriteBone   — particle positions → bone world matrices

// ---- Shared structures -----------------------------------------------------

struct Particle
{
    float3 position;
    float  invMass;
    float3 prevPosition;
    uint   boneIndex;
};

struct DistanceConstraint
{
    uint  particleA;
    uint  particleB;
    float restLength;
    float stiffness;
};

struct StrandDesc
{
    uint particleOffset;
    uint particleCount;
    uint boneOutputOffset;  // offset into bone output buffer
    uint pad;
};

// ---- Resources -------------------------------------------------------------

RWStructuredBuffer<Particle>         g_particles   : register(u0, space2);
RWStructuredBuffer<float4x4>         g_boneOutput  : register(u1, space2);

StructuredBuffer<DistanceConstraint> g_constraints : register(t0, space2);
StructuredBuffer<StrandDesc>         g_strands     : register(t1, space2);

cbuffer PhysicsConstants : register(b0, space2)
{
    float  g_dt;
    float  g_damping;
    uint   g_totalParticles;
    uint   g_totalConstraints;
    float3 g_gravity;
    uint   g_totalStrands;
    uint   g_maxBonesPerStrand;
    uint3  g_pad;
};

// ---- Pass 1: Verlet Integration -------------------------------------------

[numthreads(64, 1, 1)]
void CSIntegrate(uint3 id : SV_DispatchThreadID)
{
    uint idx = id.x;
    if (idx >= g_totalParticles) return;

    Particle p = g_particles[idx];
    if (p.invMass == 0.0f) return; // root pinned

    float3 vel    = (p.position - p.prevPosition) * g_damping;
    float3 newPos = p.position + vel + g_gravity * (g_dt * g_dt);

    g_particles[idx].prevPosition = p.position;
    g_particles[idx].position     = newPos;
}

// ---- Pass 2: PBD Distance Constraint -------------------------------------

[numthreads(64, 1, 1)]
void CSConstraint(uint3 id : SV_DispatchThreadID)
{
    uint idx = id.x;
    if (idx >= g_totalConstraints) return;

    DistanceConstraint c = g_constraints[idx];

    float3 pa   = g_particles[c.particleA].position;
    float3 pb   = g_particles[c.particleB].position;
    float3 diff = pb - pa;
    float  dist = length(diff);

    if (dist < 1e-6f) return;

    float  error      = (dist - c.restLength) / dist;
    float3 correction = diff * (error * 0.5f * c.stiffness);

    if (g_particles[c.particleA].invMass > 0.0f)
        g_particles[c.particleA].position += correction;
    if (g_particles[c.particleB].invMass > 0.0f)
        g_particles[c.particleB].position -= correction;
}

// ---- Pass 3: Write Bone Output Matrices -----------------------------------

// Build a quaternion that rotates vector 'from' to 'to'.
float4 QuatFromTwoVectors(float3 from, float3 to)
{
    float3 nf = normalize(from);
    float3 nt = normalize(to);
    float  d  = dot(nf, nt);

    if (d > 0.9999f) return float4(0, 0, 0, 1); // identity

    if (d < -0.9999f)
    {
        // 180-degree rotation: pick an arbitrary perpendicular axis
        float3 perp = abs(nf.x) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
        float3 axis = normalize(cross(nf, perp));
        return float4(axis, 0); // w=0 → 180°
    }

    float3 c = cross(nf, nt);
    float  w = 1.0f + d;
    return normalize(float4(c, w));
}

float4x4 MatFromQuatPos(float4 q, float3 pos)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;

    // Row-major, row-vector convention
    float4x4 m;
    m[0] = float4(1 - (yy + zz), xy + wz,       xz - wy,       0);
    m[1] = float4(xy - wz,       1 - (xx + zz),  yz + wx,       0);
    m[2] = float4(xz + wy,       yz - wx,        1 - (xx + yy), 0);
    m[3] = float4(pos.x,         pos.y,          pos.z,          1);
    return m;
}

[numthreads(64, 1, 1)]
void CSWriteBone(uint3 id : SV_DispatchThreadID)
{
    uint strandIdx = id.x / g_maxBonesPerStrand;
    uint boneIdx   = id.x % g_maxBonesPerStrand;

    if (strandIdx >= g_totalStrands) return;

    StrandDesc strand = g_strands[strandIdx];
    if (boneIdx >= strand.particleCount - 1) return; // last particle has no child

    uint pA = strand.particleOffset + boneIdx;
    uint pB = pA + 1;

    float3 posA = g_particles[pA].position;
    float3 posB = g_particles[pB].position;
    float3 dir  = normalize(posB - posA);

    // Bind-pose bone direction (typically pointing down the chain, -Y or +Y)
    float3 bindDir = float3(0, -1, 0);
    float4 q = QuatFromTwoVectors(bindDir, dir);

    uint outIdx = strand.boneOutputOffset + boneIdx;
    g_boneOutput[outIdx] = MatFromQuatPos(q, posA);
}
