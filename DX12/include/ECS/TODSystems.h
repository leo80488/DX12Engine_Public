#pragma once

// TODSystems.h — Time-of-Day pipeline split into five small systems
// (matches Unity DOTS / Bevy / Flecs style of one concern per system):
//
//   1. TODTickSystem            — advances TODConfig.timeOfDay
//   2. TODEvaluationSystem      — Config → Output (sun/moon dir/colour/active)
//   3. TODSunSyncSystem         — Output → LightData of SunLightTag entity
//   4. TODMoonSyncSystem        — Output → LightData of MoonLightTag entity
//   5. TODAtmosphereSyncSystem  — Output → atmosphere/sky consumers (today a
//                                 stub for future curve-driven atmosphere
//                                 params; AtmosphereComponent itself stays
//                                 author-owned)
//
// Renderer drives all five each frame in order at the top of BuildRenderScene.
//
// Singleton helpers FindConfig / FindOutput return the first entity carrying
// the matching component — the engine has no first-class singleton API, so
// "single instance" is by convention (auto-spawned on the Sky entity).

#include "ECS/ECS.h"

struct TODConfigComponent;
struct TODOutputComponent;

namespace TODUtil
{
    /** First entity's TODConfigComponent (nullptr if no Sky entity exists). */
    TODConfigComponent* FindConfig(World& w);
    /** First entity's TODOutputComponent (nullptr if not yet auto-spawned). */
    TODOutputComponent* FindOutput(World& w);
}

// Five tiny systems — kept as classes with a static Update so they slot into
// the engine's "owned by Renderer / called per frame" convention without
// requiring a registration framework.

class TODTickSystem
{
public:
    static void Update(World& w, float deltaSeconds);
};

class TODEvaluationSystem
{
public:
    static void Update(World& w);
};

class TODSunSyncSystem
{
public:
    static void Update(World& w);
};

class TODMoonSyncSystem
{
public:
    static void Update(World& w);
};

class TODAtmosphereSyncSystem
{
public:
    static void Update(World& w);
};
