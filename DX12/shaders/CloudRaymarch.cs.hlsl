// CloudRaymarch.cs.hlsl
// -----------------------------------------------------------------------------
// Quarter-resolution volumetric cloud raymarch.
//
//   1. Reconstruct world-space ray from pixel UV via invViewProj.
//   2. Sample scene depth at pixel center — clamp ray exit to opaque geometry.
//   3. Intersect view ray with horizontal cloud slab [bottomAlt, topAlt].
//   4. March front-to-back: sample density volume + cone-trace toward sun.
//   5. Accumulate premultiplied scatter + transmittance using Beer's law +
//      Henyey-Greenstein phase function.
//
// Output: RGBA16F where rgb = premultiplied in-scatter radiance, a = final
// transmittance (1 = clear sky, 0 = fully opaque cloud).
// -----------------------------------------------------------------------------

// invViewProj is uploaded transposed (engine convention — LightCB style),
// so leave the default column-major qualifier and use row-vector mul.
cbuffer CloudCB : register(b0, space2)
{
    float4x4 invViewProj;

    // Each declaration block below is 16 bytes — DO NOT reorder without
    // re-verifying the C++ CloudConstants struct in CloudPass.cpp.
    float3 cameraPos;       float nearZ;             // row 4
    float  farZ;            float bottomAltitude;
    float  topAltitude;     float coverage;          // row 5
    float  density;         float noiseScale;
    float  anisotropy;      float extinction;        // row 6

    float3 sunDir;          float ambientStrength;   // row 7
    float3 sunColor;        float _pad0;             // row 8
    float3 cloudColor;      float _pad1;             // row 9
    float3 windOffset;      float _pad2;             // row 10

    float  halfResW;        float halfResH;
    float  fullResW;        float fullResH;          // row 11
};

Texture3D<float>          NoiseTex   : register(t0, space2);
Texture2D<float>          SceneDepth : register(t3, space2);
// Engine's static s0 space2 is CLAMP — we wrap manually with frac() in
// SampleDensity to make the tileable noise volume repeat across the sky.
SamplerState              LinearClamp : register(s0, space2);

RWTexture2D<float4>       CloudOut   : register(u0, space2);

#define RM_STEPS         64
#define RM_LIGHT_STEPS    6
#define RM_LIGHT_DIST   150.0   // metres per light step (cone-trace)

// Horizon clamp — without this, near-horizontal rays slice the cloud slab
// for tens of kilometres and the integrated density turns into a solid
// band that visually "clumps" all distant clouds together. Limit total
// march length and feather density toward the horizon distance.
#define RM_MAX_DIST     20000.0
#define RM_FADE_BEGIN   12000.0

// ---------------------------------------------------------------------------
float HenyeyGreenstein(float cosT, float g)
{
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosT;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

// ---------------------------------------------------------------------------
// Slab intersect — ray P + t*D with y-planes [yLo, yHi]. Returns true if the
// ray passes through the slab, with entry/exit distances along the ray.
bool IntersectSlab(float3 P, float3 D, float yLo, float yHi,
                   out float tEntry, out float tExit)
{
    tEntry = 0.0; tExit = 1e9;
    if (abs(D.y) < 1e-4)
    {
        // Ray is parallel — entirely inside slab or entirely outside.
        if (P.y < yLo || P.y > yHi) return false;
        return true;
    }
    float t0 = (yLo - P.y) / D.y;
    float t1 = (yHi - P.y) / D.y;
    tEntry = max(0.0, min(t0, t1));
    tExit  = max(t0, t1);
    return tExit > tEntry;
}

// ---------------------------------------------------------------------------
// Sample the cloud density at a world position. Includes wind offset + coverage
// threshold + density multiplier + altitude falloff at the slab edges.
float SampleDensity(float3 wp)
{
    float3 samplePos = frac((wp + windOffset) * noiseScale);
    float n = NoiseTex.SampleLevel(LinearClamp, samplePos, 0);

    // Coverage threshold — pixels below `1-coverage` are erased.
    float d = saturate(n - (1.0 - coverage));

    // Altitude-based shape: fade at top and bottom so the slab edges aren't
    // hard rectangles. Peak around the middle of the slab.
    float h = saturate((wp.y - bottomAltitude) / max(1.0, topAltitude - bottomAltitude));
    float verticalProfile = saturate(h * 4.0) * saturate((1.0 - h) * 4.0);
    d *= verticalProfile;

    return d * density;
}

// Cone-trace toward sun: short march sampling density at increasing steps so
// nearby features dominate but distant features still contribute.
float SunLightTransmittance(float3 wp)
{
    float opticalDepth = 0.0;
    [unroll] for (int i = 0; i < RM_LIGHT_STEPS; ++i)
    {
        // Increasing step length so far samples cover more ground.
        float stepLen = RM_LIGHT_DIST * (1.0 + float(i) * 0.5);
        float3 sp = wp + sunDir * stepLen * (float(i) + 0.5);
        opticalDepth += SampleDensity(sp) * stepLen * extinction;
    }
    return exp(-opticalDepth);
}

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint2 dt : SV_DispatchThreadID)
{
    if (dt.x >= (uint)halfResW || dt.y >= (uint)halfResH) return;

    // Reconstruct world-space ray. UV → NDC → world (via invViewProj).
    float2 uv  = (float2(dt) + 0.5) / float2(halfResW, halfResH);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    // Reversed-Z: near plane is NDC z=1, far is NDC z=0.
    float4 nearW = mul(float4(ndc, 1.0, 1.0), invViewProj);
    float4 farW  = mul(float4(ndc, 0.0, 1.0), invViewProj);
    float3 rayP  = nearW.xyz / nearW.w;
    float3 rayQ  = farW.xyz  / farW.w;
    float3 rayD  = normalize(rayQ - rayP);

    // Looking down through the bottom of the slab → no cloud (camera below).
    // Looking up through clouds is also fine.
    float tEntry, tExit;
    if (!IntersectSlab(cameraPos, rayD, bottomAltitude, topAltitude, tEntry, tExit))
    {
        CloudOut[dt] = float4(0, 0, 0, 1);
        return;
    }

    // Clamp ray exit to scene depth — opaque geometry occludes clouds.
    // Sample the centre of the full-res pixel cluster covered by this output texel.
    float2 fullUv = (float2(dt) + 0.5) / float2(halfResW, halfResH);
    int2 dpx = int2(fullUv * float2(fullResW, fullResH));
    float sceneNdcZ = SceneDepth.Load(int3(dpx, 0));
    // Reversed-Z: ndcZ=1 → near, 0 → far. Linear distance via the projection.
    // Use the linearised distance: linDist = nearZ * farZ / (farZ - ndcZ * (farZ - nearZ)).
    float sceneDist = 1e9;
    if (sceneNdcZ > 0.0)
    {
        // Linear view-space depth (reversed-Z, infinite-far-not-assumed).
        float linZ = (nearZ * farZ) / (farZ - sceneNdcZ * (farZ - nearZ));
        // Distance along ray ~= linZ / dot(rayD, cameraForward). Cameraforward
        // is not available here, but for tight FOV the approximation linZ /
        // dot(rayD, forward) ≈ linZ for centre rays; for off-axis rays the
        // approximation under-shoots. Acceptable for an MVP — the clouds get
        // capped at slightly conservative depths, never bleed through opaque.
        sceneDist = linZ;
    }
    tExit = min(tExit, sceneDist);
    // Hard horizon cap — without this, near-horizontal rays accumulate density
    // across tens of km and the result is a thick continuous band.
    tExit = min(tExit, RM_MAX_DIST);
    if (tExit <= tEntry)
    {
        CloudOut[dt] = float4(0, 0, 0, 1);
        return;
    }

    // March from entry to exit.
    const float marchLen = tExit - tEntry;
    const float stepLen  = marchLen / float(RM_STEPS);
    float3 p             = cameraPos + rayD * tEntry;

    float cosT      = dot(rayD, sunDir);
    float phase     = HenyeyGreenstein(cosT, anisotropy);
    float ambient   = ambientStrength;

    float3 scatter        = 0;
    float  transmittance  = 1.0;

    [loop] for (int i = 0; i < RM_STEPS; ++i)
    {
        float d = SampleDensity(p);

        // Distance fade — feather density toward the horizon so far clouds
        // disappear rather than smearing into a solid wall.
        float rayDist = tEntry + (float(i) + 0.5) * stepLen;
        float distFade = 1.0 - smoothstep(RM_FADE_BEGIN, RM_MAX_DIST, rayDist);
        d *= distFade;

        if (d > 0.001)
        {
            float sigmaT     = d * extinction;       // extinction coefficient
            float stepT      = exp(-sigmaT * stepLen);

            // Sun-direction transmittance (cone trace) + ambient skylight fill.
            float sunT       = SunLightTransmittance(p);
            float3 directL   = sunColor * (sunT * phase);
            float3 ambientL  = sunColor * ambient * 0.25;
            float3 L         = (directL + ambientL) * cloudColor;

            // Integrate scattering: in-scatter mass = (1 - stepT) / sigmaT,
            // weighted by current transmittance. Drop the σ_s factor — folded
            // into density.
            float3 integ     = L * (1.0 - stepT);
            scatter         += transmittance * integ;
            transmittance   *= stepT;

            if (transmittance < 0.01) { transmittance = 0; break; }
        }
        p += rayD * stepLen;
    }

    CloudOut[dt] = float4(scatter, transmittance);
}
