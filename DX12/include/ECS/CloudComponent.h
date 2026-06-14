#pragma once

// CloudComponent — Volumetric cloud authoring knobs (singleton).
//
// Sits on the Sky entity (or any single entity — Renderer scans for the
// first instance each frame). Drives CloudPass: a quarter-res raymarch
// through a spherical cloud shell [bottomAltitude, topAltitude] using the
// Nubis density model (Perlin-Worley base + weather map + per-type height
// gradient + Worley detail erosion) and Frostbite lighting (multi-scatter
// octaves, dual-lobe HG + silver lining, energy-conserving integration).
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
    // Global coverage. 0 = clear sky, 0.5 = the weather map's distinct
    // formations as authored, 1 = overcast (secondary fill field floods in).
    float       coverage  = 0.50f;
    // Multiplier on density after shaping. Higher = thicker clouds.
    float       density   = 1.0f;
    // World-units -> base-shape sample period. 0.00025 = 4 km per tile.
    float       noiseScale = 0.00025f;
    // World-units -> detail-erosion sample period. 0.002 = 500 m per tile.
    float       detailNoiseScale = 0.002f;
    // How deep the high-frequency erosion eats into cloud edges (0 = none).
    float       detailStrength = 0.30f;
    // World-units -> weather-map period. 0.00002 = 50 km per tile.
    float       weatherScale = 0.00002f;
    // Shifts the weather map's cloud type: -1 = force stratus, 0 = as
    // authored, +1 = force cumulus.
    float       cloudTypeBias = 0.0f;
    // 0 = none, 1 = full cumulonimbus anvil spread near the layer top.
    float       anvilBias = 0.0f;

    // ---- Wind --------------------------------------------------------------
    // Wind direction (does not have to be normalised). Multiplied by speed *
    // time and added to the noise sample position.
    DirectX::XMFLOAT3 windDirection { 1.0f, 0.0f, 0.0f };
    float             windSpeed     = 8.0f;   // m/s

    // ---- Lighting ----------------------------------------------------------
    // Dual-lobe Henyey-Greenstein: forward lobe g (silver lining when the
    // sun is behind the cloud) ...
    float       anisotropy = 0.6f;
    // ... blended with a backward lobe so the anti-solar side keeps shape.
    float       phaseBackG = -0.2f;
    float       phaseBlend = 0.3f;
    // Extra forward "silver lining" lobe, combined with max() (Nubis).
    float       silverIntensity = 0.8f;
    float       silverSpread    = 0.25f;   // lobe g = 0.99 - spread
    // Beer's-law extinction coefficient (1/m). Real water clouds measure
    // 0.04-0.12; density multiplies on top.
    float       extinction = 0.05f;
    // Ambient skylight fill, scaled by sun luminance (tracks time-of-day)
    // and a bottom-occlusion height gradient.
    float       ambientStrength = 0.5f;
    DirectX::XMFLOAT3 ambientTint { 0.55f, 0.65f, 0.85f };
    // Base cloud albedo tint. White by default (clouds get their warmth from
    // the sun colour driven by TOD).
    DirectX::XMFLOAT3 cloudColor { 1.0f, 1.0f, 1.0f };

    // ---- Quality -----------------------------------------------------------
    // Max view-ray samples (reached once the in-shell segment spans 15 km).
    float       maxSteps = 96.0f;
};
