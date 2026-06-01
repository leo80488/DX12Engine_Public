#pragma once

// AtmosphereComponent — atmospheric / IBL / aerial / star knobs only.
//
// Time-of-Day no longer lives here. Sun/Moon direction, time, latitude,
// brightness, and moon intensity moved to [[TODComponents.h]]
// (TODConfigComponent + TODOutputComponent + SunLightTag + MoonLightTag).
//
// Renderer pushes these fields into SkyIBLPass each frame; SaveScene serialises
// them. Pairs with [[SkyboxComponent]] on the same entity for static IBL
// cubemaps when skyboxSource == Static.

#include <cstdint>
#include <DirectXMath.h>

struct AtmosphereComponent
{
    // ---- Master ------------------------------------------------------------
    // Procedural atmosphere on/off. When off, SkyIBLPass falls back to the
    // static IBL cubemaps from SkyboxComponent.
    bool        atmosphereEnabled = true;

    // Backdrop source for SkyboxPass.
    //   Atmosphere = procedural sky cubemap from Hillaire LUTs
    //   Static     = .itex referenced by paired SkyboxComponent
    enum class SkyboxSource : uint8_t { Atmosphere = 0, Static = 1 };
    SkyboxSource skyboxSource = SkyboxSource::Atmosphere;

    // ---- IBL ---------------------------------------------------------------
    // Master scale on SKY-DERIVED indirect lighting (Unreal-style "Sky Light
    // Intensity"): gates sky-IBL diffuse + reflection-probe/sky specular
    // uniformly. 0 = no visible-sky indirect contribution.
    //
    // DOES NOT affect DDGI (decoupled 2026-05-23). DDGI is its own indirect
    // path with its own master scale — see IndirectLightingSettings
    // .ddgiDiffuseScale. Use that to silence/boost DDGI independently.
    //
    // skyIBLDiffuseScale composes BEFORE iblStrength (per-source weight ×
    // master gate). DDGI's ddgiDiffuseScale is the sole DDGI multiplier.
    float       iblStrength   = 0.1f;

    // ---- Aerial Perspective ------------------------------------------------
    // Distance-fog composite in LightingPass. Off by default — depends on
    // the scene's world-unit-to-km scale.
    bool        aerialCompositeEnabled = false;

    // ---- Stars -------------------------------------------------------------
    // Stars per cubemap face (procedural star bake density).
    float       starDensity    = 256.0f;
    // Star brightness multiplier (0..1 typical).
    float       starBrightness = 0.7f;

    // Note: volumetric fog sun strength is OWNED BY THE DIRECTIONAL LIGHT —
    // attach a VolumetricLightComponent to a LightData(Directional) entity
    // to opt that light into fog god-rays. AtmosphereComponent intentionally
    // does NOT carry a fog-strength field; the light owns its own fog
    // contribution.
};
