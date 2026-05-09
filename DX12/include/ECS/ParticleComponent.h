#pragma once

// ParticleEmitterComponent — CPU-side settings for a GPU particle emitter.
//
// v2 adds:
//   - Shape (Point/Sphere/Cone/Box/Circle/Mesh) → drives spawn position+velocity
//   - Visual mode (Flat/Fire/Smoke/Electric) → drives PS look
//   - Texture slot (optional SRV bindless index)
//   - Mesh source entity (for ParticleShape::Mesh — samples surface triangles)

#include "ECS/ECS.h"
#include <cstdint>
#include <string>
#include <DirectXMath.h>

enum class ParticleBlendMode : uint8_t
{
    Alpha    = 0,  // SRC_ALPHA / INV_SRC_ALPHA
    Additive = 1,  // ONE / ONE (sparks, magic glow)
};

// Spawn geometry. Each shape samples position + velocity differently.
// See shaders/ParticleEmit.cs.hlsl for the per-shape math.
enum class ParticleShape : uint8_t
{
    Point   = 0,  // spawn at emitter.position; velocity ~ velMin..velMax
    Sphere  = 1,  // sphere surface or volume
    Cone    = 2,  // cone with half-angle + direction
    Box     = 3,  // AABB volume
    Circle  = 4,  // disk facing `circleNormal`
    Mesh    = 5,  // random point on mesh source entity's surface triangles
};

// Visual style applied in the pixel shader. Composable with texture slot.
enum class ParticleVisualMode : uint8_t
{
    Flat     = 0,  // solid color * radial alpha falloff
    Fire     = 1,  // heat ramp (black→red→orange→yellow→white)
    Smoke    = 2,  // soft desaturated, alpha from age²
    Electric = 3,  // high-contrast emissive pulse
};

struct ParticleEmitterComponent
{
    // ---- Spawn rate --------------------------------------------------------
    float             spawnRate        = 50.0f;

    // ---- Per-particle initial state ---------------------------------------
    float             startLifetime    = 2.0f;
    float             startSize        = 0.2f;

    DirectX::XMFLOAT3 velocityMin      = { -0.5f,  1.0f, -0.5f };
    DirectX::XMFLOAT3 velocityMax      = {  0.5f,  3.0f,  0.5f };

    DirectX::XMFLOAT4 startColor       = { 1.0f, 0.8f, 0.3f, 1.0f };
    DirectX::XMFLOAT4 endColor         = { 1.0f, 0.2f, 0.1f, 0.0f };

    DirectX::XMFLOAT3 gravity          = { 0.0f, -9.8f, 0.0f };

    // ---- Render mode ------------------------------------------------------
    ParticleBlendMode  blendMode       = ParticleBlendMode::Additive;
    ParticleVisualMode visualMode      = ParticleVisualMode::Flat;

    // Optional texture. `textureBindlessIdx` is what the shader actually
    // samples via the bindless table; CPU resolves via TextureSystem on
    // load. < 0 (or 0xFFFFFFFF) = no texture, shader falls back to radial
    // alpha mask.
    int32_t           textureBindlessIdx = -1;
    std::string       texturePath;          // for serialization + editor display
    uint64_t          textureGpuHandle   = 0;  // preview handle for inspector swatch

    // ---- Shape ------------------------------------------------------------
    ParticleShape     shape              = ParticleShape::Point;

    // Sphere params
    float             sphereRadius       = 1.0f;
    bool              sphereSpawnOnShell = true;   // true = surface, false = volume

    // Cone params
    float             coneHalfAngle      = 0.5236f;           // 30 degrees
    DirectX::XMFLOAT3 coneDirection      = { 0.0f, 1.0f, 0.0f };
    float             coneLength         = 1.0f;              // optional forward offset

    // Box params
    DirectX::XMFLOAT3 boxHalfExtents     = { 1.0f, 1.0f, 1.0f };

    // Circle params
    float             circleRadius       = 1.0f;
    DirectX::XMFLOAT3 circleNormal       = { 0.0f, 1.0f, 0.0f };

    // Mesh params — the entity whose MeshLibRef / MeshHandle provides the
    // triangle surface. CPU resolves each frame: looks up MeshDescriptor +
    // copies the entity's GlobalTransform into the GPU emitter record.
    Entity            meshSourceEntity   = 0;  // NullEntity; 0 = unset

    // ---- Emitter control --------------------------------------------------
    bool              enabled          = true;

    // ---- CPU-only per-frame state -----------------------------------------
    float             spawnAccumulator = 0.0f;
};
