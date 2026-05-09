#ifndef ATMOSPHERE_COMMON_HLSLI
#define ATMOSPHERE_COMMON_HLSLI

// -----------------------------------------------------------------------------
// AtmosphereCommon.hlsli
//
// Shared constants + helper functions for the Hillaire 2020 LUT pipeline:
//   - TransmittanceLUT (256×64, once)       : transmittance from (altitude, μ) to TOA
//   - MultiScatterLUT  (32×32,  once)       : 2nd+-order scatter factor
//   - SkyViewLUT       (192×108, per-frame) : sky radiance by (azimuth, elevation)
//
// All distances in km.
// -----------------------------------------------------------------------------

#define ATMO_PI       3.14159265358979323846
#define ATMO_INV_PI   0.31830988618379067154

// ---- Earth-scale constants (Hillaire 2020 defaults) -------------------------
static const float  kGroundR      = 6360.0;  // planet radius (km)
static const float  kAtmoR        = 6460.0;  // atmosphere outer radius (km)

static const float3 kRayleighScat = float3(5.802, 13.558, 33.1) * 1e-3;
static const float  kRayleighH    = 8.0;

static const float  kMieScat      = 3.996e-3;
static const float  kMieAbsorb    = 4.4e-3;
static const float  kMieH         = 1.2;
static const float  kMiePhaseG    = 0.92;

static const float3 kOzoneAbsorb  = float3(0.650, 1.881, 0.085) * 1e-3;
static const float  kOzoneCenter  = 25.0;
static const float  kOzoneHalfWid = 15.0;

static const float3 kGroundAlbedo = float3(0.3, 0.3, 0.3);

// ---- Density / coefficient sampling -----------------------------------------
struct AtmoMedium
{
    float3 scattering;  // Rayleigh + Mie (per-channel)
    float3 extinction;  // scattering + absorption (+ Ozone absorption)
    float3 sctRayleigh; // Rayleigh only (for phase weighting)
    float  sctMie;      // Mie only
};

AtmoMedium SampleAtmosphere(float altitude)
{
    // altitude is km above ground (can be negative underground — clamp).
    float h   = max(altitude, 0.0);
    float rDens = exp(-h / kRayleighH);
    float mDens = exp(-h / kMieH);
    float oDens = saturate(1.0 - abs(h - kOzoneCenter) / kOzoneHalfWid);

    AtmoMedium a;
    a.sctRayleigh = kRayleighScat * rDens;
    a.sctMie      = kMieScat * mDens;
    a.scattering  = a.sctRayleigh + float3(a.sctMie, a.sctMie, a.sctMie);
    a.extinction  = a.scattering
                  + float3(kMieAbsorb, kMieAbsorb, kMieAbsorb) * mDens
                  + kOzoneAbsorb * oDens;
    return a;
}

// ---- Phase functions --------------------------------------------------------
float RayleighPhase(float cosT)
{
    return 3.0 / (16.0 * ATMO_PI) * (1.0 + cosT * cosT);
}
float MiePhaseHG(float cosT, float g)
{
    float g2   = g * g;
    float num  = (1.0 - g2) * (1.0 + cosT * cosT);
    float den  = (2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-3), 1.5);
    return num / (4.0 * ATMO_PI * den);
}

// ---- Sphere intersection ----------------------------------------------------
// Ray origin `ro` inside sphere (or allow outside). Returns nearest positive
// intersection distance, or -1 if no hit in front.
float RaySphere(float3 ro, float3 rd, float radius)
{
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float d = sqrt(disc);
    float t0 = -b - d;
    float t1 = -b + d;
    if (t1 < 0.0) return -1.0;
    return (t0 < 0.0) ? t1 : t0;
}

// Returns distance to atmosphere top along `rd` from point `ro` that is
// inside the atmosphere. Returns -1 if the ray points down through the ground
// before reaching the atmosphere boundary.
float DistanceToAtmosphereTop(float3 ro, float3 rd)
{
    float tTop   = RaySphere(ro, rd, kAtmoR);
    float tGround = RaySphere(ro, rd, kGroundR);
    if (tGround > 0.0 && tGround < tTop) return -1.0; // hits ground first
    return tTop;
}

// ---- LUT parameterisations (Hillaire 2020) ----------------------------------
// Transmittance LUT maps (height h in [0, atmosphereThickness], cos(sun zenith)
// μ in [-1, 1]) → uv in [0, 1]^2.

void ParamsToLutUv(float h, float mu, out float2 uv)
{
    // Height encoded as sqrt for density sensitivity near ground.
    float H = sqrt(kAtmoR * kAtmoR - kGroundR * kGroundR);
    float rho = sqrt(max(0.0, h * (2.0 * kGroundR + h)));
    float u   = rho / H;

    // Parameterise mu by horizon angle for non-linear precision near the horizon.
    float r = kGroundR + h;
    float d_min = kAtmoR - r;
    float d_max = rho + H;
    float d     = max(0.0, -r * mu + sqrt(max(0.0, r * r * (mu * mu - 1.0) + kAtmoR * kAtmoR)));
    float v     = (d - d_min) / max(d_max - d_min, 1e-6);
    uv = float2(u, v);
}

void LutUvToParams(float2 uv, out float h, out float mu)
{
    float H = sqrt(kAtmoR * kAtmoR - kGroundR * kGroundR);
    float rho = H * uv.x;
    h = sqrt(rho * rho + kGroundR * kGroundR) - kGroundR;

    float r = kGroundR + h;
    float d_min = kAtmoR - r;
    float d_max = rho + H;
    float d = d_min + uv.y * (d_max - d_min);
    mu = (d == 0.0) ? 1.0
                    : (H * H - rho * rho - d * d) / max(2.0 * r * d, 1e-6);
    mu = clamp(mu, -1.0, 1.0);
}

// SkyView LUT non-linear elevation parameterisation (more precision near horizon).
//   elevation ∈ [-π/2, π/2], uv.y ∈ [0, 1]
float SkyViewElevationToV(float elevation)
{
    // Hillaire uses `sign * sqrt(|x|)` style to pull horizon to mid.
    float sgn = sign(elevation);
    float a   = sqrt(abs(elevation) / (ATMO_PI * 0.5));
    return 0.5 + 0.5 * sgn * a;
}
float SkyViewVToElevation(float v)
{
    float c   = 2.0 * v - 1.0;
    float sgn = sign(c);
    return sgn * c * c * (ATMO_PI * 0.5);
}

#endif // ATMOSPHERE_COMMON_HLSLI
