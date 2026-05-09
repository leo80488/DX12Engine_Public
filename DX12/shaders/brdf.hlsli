#ifndef BRDF_HLSLI
#define BRDF_HLSLI

// brdf.hlsli — Cook-Torrance microfacet BRDF functions.
// Shared by Lighting.ps.hlsl and Transparent.ps.hlsl.

static const float PI = 3.14159265358979;

// Fresnel-Schlick approximation.
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Fresnel with roughness bias — for IBL specular at grazing angles.
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 r = float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness);
    return F0 + (max(r, F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// GGX normal distribution function.
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-7);
}

// Schlick-GGX geometry term (single direction).
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
}

// Smith geometry term (both light and view directions).
float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness)
         * GeometrySchlickGGX(NdotL, roughness);
}

// Karis 2013 analytical BRDF approximation (split-sum without LUT texture).
// EnvBRDF(F0, roughness, NdotV) ~ F0 * A + B
float2 EnvBRDFApprox(float roughness, float NdotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float  a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// Compute full Cook-Torrance specular + energy-conserving diffuse weight.
// Returns: specular term in outSpecular, diffuse weight kD.
void EvalCookTorrance(float3 N, float3 V, float3 L, float3 F0,
                      float roughness, float metalness, float3 albedo,
                      out float3 outSpecular, out float3 outKD)
{
    float3 H     = normalize(V + L);
    float  NdotL = saturate(dot(N, L));
    float  NdotV = saturate(dot(N, V));

    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
    float  D = DistributionGGX(N, H, roughness);
    float  G = GeometrySmith(NdotV, NdotL, roughness);

    outSpecular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    outKD       = (1.0 - F) * (1.0 - metalness);
}

#endif // BRDF_HLSLI
