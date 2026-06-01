#pragma once

// TODComponents.h — Time-of-Day singleton pattern.
//
// Two-component split (Unity DOTS / Bevy Resource style):
//   TODConfigComponent  : INPUT  — author-set knobs (time, speed, latitude, ...).
//   TODOutputComponent  : OUTPUT — TODEvaluationSystem-written computed state
//                                  (sun/moon dir, color, isMoonActive, ...).
//
// "Singleton" here means: only ONE entity in the world carries these
// components. Renderer auto-spawns one on the Sky entity if none exists.
// Helpers in TODSystems.h locate the first such entity each frame.
//
// Tag components SunLightTag / MoonLightTag mark the LightData entities that
// TOD sync-systems drive. Use as a sentinel: any entity that has the tag is
// the canonical sun (or moon) directional light.

#include <cstdint>
#include <DirectXMath.h>

// ---------------------------------------------------------------------------
// INPUT — author-set parameters. Persisted via SaveScene.
// ---------------------------------------------------------------------------
struct TODConfigComponent
{
    // Master on/off. When false, TOD systems leave outputs untouched so the
    // scene can be lit by static directional lights as authored.
    bool        enabled = false;

    // 0=midnight, 0.25=sunrise, 0.5=noon, 0.75=sunset. Wraps mod 1.
    // Advanced by TODTickSystem each frame when enabled && timeSpeed!=0.
    float       timeOfDay = 0.35f;

    // Day units per real second. 1/60 ≈ 1 real minute per in-game day.
    float       timeSpeed = 0.0f;

    // Geographic latitude in radians; controls how high the sun climbs.
    float       latitudeRad = 0.6f;

    // Master sun-brightness multiplier. Applied to BOTH the geometric sun
    // (atmosphere scattering) and the active body (direct lighting + IBL).
    float       sunBrightnessScale = 1.0f;

    // Moon brightness relative to sun's basePeak (≈1.5% by default).
    float       moonIntensityScale = 0.015f;
};

// ---------------------------------------------------------------------------
// OUTPUT — computed per frame by TODEvaluationSystem.
// NOT serialized — recomputed from Config on load.
// ---------------------------------------------------------------------------
struct TODOutputComponent
{
    // "Active body" — direction & colour the rest of the engine treats as
    // THE directional light. This is the SUN by day, MOON by night.
    DirectX::XMFLOAT3 activeDirection { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3 activeColor     { 10.0f, 10.0f, 10.0f };

    // Geometric sun — true position even below horizon, colour zero below
    // horizon. The atmosphere / aerial-perspective shaders sample this so
    // the sky doesn't compute Rayleigh from the moon at night.
    DirectX::XMFLOAT3 sunDirection { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3 sunColor     { 10.0f, 10.0f, 10.0f };

    // Moon — sits opposite the sun, fades smoothly across horizon.
    DirectX::XMFLOAT3 moonDirection { 0.0f, -1.0f, 0.0f };
    DirectX::XMFLOAT3 moonColor     { 0.055f, 0.070f, 0.100f };

    // True when sun is below horizon and moon drives the active lighting.
    bool isMoonActive   = false;
    // True when the moon disk should be drawn (slightly past horizon for fade).
    bool moonDiskVisible = false;

    // Set true by TODEvaluationSystem any frame the outputs changed.
    // Read by downstream sync systems to skip work when nothing moved.
    bool dirty = true;
};

// ---------------------------------------------------------------------------
// Tag components — empty markers. Place on the LightData(Directional) entity
// that should be driven by TOD for sun / moon roles, respectively.
// ---------------------------------------------------------------------------
struct SunLightTag  {};
struct MoonLightTag {};
