#pragma once

// ShadingModel — UE-style "what kind of surface is this?" enum.
//
// The engine's lighting pass uses this to decide how to interpret the
// GBuffer output that came out of the pixel shader. Every custom material
// shader MUST pick exactly one ShadingModel; the shader's job is to produce
// the GBuffer layout expected by that model, and the lighting pass then
// does the model-specific BRDF / scatter / etc.
//
// Phase A (this header): just the enum + name helper. No mapping to
// GBuffer layouts yet — that contract lives in Phase B on the GBufferPass
// side, where the lighting pass will read a packed `shadingModelID` byte
// out of one of the GBuffer RTs and branch accordingly.
//
// The enum is intentionally FIXED. Adding a new shading model requires
// (1) adding a value here, (2) extending the lighting shader's switch,
// (3) documenting the GBuffer contract below. We don't do asset-defined
// shading models because the GBuffer layout is fixed 5-RT and the lighting
// shader needs to understand each model's semantics to evaluate lighting
// correctly.

#include <cstdint>

enum class ShadingModel : uint8_t
{
    // Standard microfacet PBR with metallic/roughness workflow. Uses the
    // current default GBuffer layout verbatim:
    //   RT0 baseColor.rgb       / alpha unused
    //   RT1 worldNormal.xyz     / shadingModelID.w (packed)
    //   RT2 (roughness, metalness, AO, reflectance)
    //   RT3 motion vector
    //   RT4 emissive HDR
    Standard = 0,

    // No lighting — baseColor goes straight to the scene color, emissive
    // treated as pass-through. GBuffer normals/surface still written for
    // things like TAA / motion but the lighting pass early-outs.
    Unlit,

    // Two-layer specular: base lobe + clearcoat lobe on top. Requires
    // extra surface channels (clearcoat strength + clearcoat roughness).
    // Phase B will repurpose unused bits of RT2.a / RT1.a for these; exact
    // encoding TBD when the lighting pass lands.
    ClearCoat,

    // Subsurface scattering — diffuse lighting gets blurred / tinted by a
    // subsurface color. Used for skin, wax, leaves. Phase B packs subsurface
    // color + profile index into an otherwise-unused channel.
    Subsurface,

    // Anisotropic GGX — directional specular highlight along a tangent
    // frame (hair, brushed metal). Phase B writes tangent.xyz + anisotropy
    // scalar into spare GBuffer channels.
    Anisotropic,

    Count
};

// Returns a stable string form suitable for Inspector display / logging.
// Never returns nullptr.
const char* ShadingModelName(ShadingModel m);
