#pragma once

// CloudComponent — Volumetric cloud authoring knobs (singleton).
//
// Sits on the Sky entity (or any single entity — Renderer scans for the
// first instance each frame). Drives CloudPass: a quarter-res raymarch
// through a single 3D Worley/Perlin noise volume between [bottomAltitude,
// topAltitude].
//
// TOD coupling: sun direction + sun colour come from TODOutputComponent
// each frame (Renderer reads, pushes to CloudPass). Cloud author values
// here stay independent of TOD.

#include <cstdint>
#include <DirectXMath.h>

struct CloudComponent
{
    // ---- Master ------------------------------------------------------------
    bool        enabled = false;

    // ---- Layer geometry (world-space metres) ------------------------------
    float       bottomAltitude = 1500.0f;
    float       topAltitude    = 4000.0f;

    // ---- Shape -------------------------------------------------------------
    // 0=no clouds, 1=full overcast. Threshold applied to baked noise.
    float       coverage  = 0.40f;
    // Multiplier on density after coverage threshold. Higher = thicker clouds.
    float       density   = 0.5f;
    // World-units → noise sample period. Lower = larger cloud features,
    // longer tile period (less visible repetition toward horizon).
    // 0.0003 ≈ 3.3 km per tile — comfortable for horizon-grazing views.
    float       noiseScale = 0.00025f;

    // ---- Wind --------------------------------------------------------------
    // Wind direction (does not have to be normalised). Multiplied by speed *
    // time and added to the noise sample position.
    DirectX::XMFLOAT3 windDirection { 1.0f, 0.0f, 0.0f };
    float             windSpeed     = 8.0f;   // m/s

    // ---- Lighting ----------------------------------------------------------
    // Henyey-Greenstein phase function asymmetry, -1..1. Positive = forward
    // scattering (silver lining when sun is behind).
    float       anisotropy = 0.55f;
    // Beer's-law extinction coefficient — scales density into optical depth.
    float       extinction = 0.04f;
    // Constant ambient term (skylight fill) applied to scatter regardless of
    // sun direction. Prevents clouds going pitch-black at noon when looking
    // straight up the sun.
    float       ambientStrength = 0.35f;
    // Base cloud albedo tint. White by default (clouds get their warmth from
    // the sun colour driven by TOD).
    DirectX::XMFLOAT3 cloudColor { 1.0f, 1.0f, 1.0f };
};
