#pragma once

#include <DirectXMath.h>

// HeightFogComponent — UE-style exponential height fog (analytic, fullscreen).
//
// Lives on the Sky entity next to AtmosphereComponent / CloudComponent
// (auto-attached DISABLED by Renderer_IBL::SyncSkyboxIBL so it shows in the
// inspector without changing existing scenes). Renderer pushes it into
// HeightFogPass each frame; sun direction/colour come from LightCB, which is
// TOD-authoritative.
//
// density(h) = fogDensity * exp(-fogHeightFalloff * (h - fogHeight)); the
// pass integrates the closed form along each view ray, including sky pixels
// (analytic t→∞ limit), so the horizon fogs like UE's exponential height fog.
// All distances/heights in world metres.
struct HeightFogComponent
{
    bool  enabled            = false;

    float fogDensity         = 0.002f;   // extinction at h == fogHeight (1/m)
    float fogHeightFalloff   = 0.02f;    // 1/m — density halves every ~35 m above fogHeight
    float fogHeight          = 0.0f;     // world Y (m) where density == fogDensity
    float startDistance      = 0.0f;     // fog-free metres in front of the camera
    float maxOpacity         = 1.0f;     // UE FogMaxOpacity — clamp on (1 - transmittance)

    DirectX::XMFLOAT3 fogColor { 0.45f, 0.55f, 0.70f }; // ambient inscatter (linear)
    float sunInscatterIntensity = 1.0f;  // scale on the HG sun term; 0 = off
    float anisotropy            = 0.7f;  // HG g (forward scattering toward the sun)
};
