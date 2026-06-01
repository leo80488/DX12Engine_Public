#pragma once

// VFXPrefab — disk-serializable VFX template: one root + N HETEROGENEOUS emitter
// entries. A single prefab (fired by one Timeline VFX notify) can spawn a
// coordinated set of effects — particle + trail + beam + decal + tracer +
// afterimage + mesh — simultaneously or staggered via per-emitter startDelay.
//
// File format (`.ivfx`, plain text, line-based KV):
//
//     VFX 1                        # format version (v1 PARTICLE-only files still load)
//     DURATION 1.5                 # optional default duration (sec); overridden
//                                  # by PendingVFXSpawns::Spawn.duration if > 0,
//                                  # and by a per-emitter DUR line if >= 0.
//     EMITTER                      # begin a new emitter entry; defaults to PARTICLE
//     P 0 0.5 0                    # local-space offset (x y z)
//     R 0 0 0                      # local-space rotation, Euler degrees (x y z)
//     S 1 1 1                      # local scale
//     PARTICLE spawnRate=50.0 startLifetime=2.0 ...   # KV blob for the particle emitter
//
//     EMITTER TRAIL                # type token directly on the EMITTER line ...
//     TRAIL width=0.12 maxAge=0.4 startColor=1_0.9_0.4_1 endColor=1_0.3_0.1_0
//
//     EMITTER                      # ... or a standalone TYPE directive
//     TYPE BEAM
//     DELAY 0.08                   # per-emitter stagger (sec); 0 = fire with the rest
//     BEAM radiusScale=1 wobbleAmp=0.05 shader=BeamTube_OuterGlow.ps.hlsl blend=additive
//     BP -1 0 0 0.3                # beam control point: x y z radius (>= 2 needed)
//     BP  3 0 0 0.3
//
//     EMITTER DECAL
//     DECAL material=scorch_a sizeX=0.5 sizeY=0.5 depth=0.2 life=8 fade=0.5
//
//     EMITTER TRACER
//     TRACER len=8 width=0.04 color=2_0.6_0.2_3 life=0.3
//
//     EMITTER AFTERIMAGE           # applies to the SPAWNER skinned character itself
//     AFTERIMAGE life=0.4 color=0.4_0.7_1_1
//
//     EMITTER MESH
//     MESH mesh=FX/shockwave.meshlib material=FX/shockwave.imat
//
// All numeric tokens are float; quaternion is reconstructed from Euler so
// designers can hand-author files without quaternion math. Vector KV values
// use underscores ("1_0.9_0.4_1"), matching the ComponentSerializer convention.
//
// Backward compatibility: an EMITTER with no type token defaults to PARTICLE,
// and the loader silently ignores unknown directives, so every existing
// PARTICLE-only .ivfx keeps parsing byte-for-byte.
//
// Loader is intentionally trusting — malformed files log a warning and
// degrade to "empty prefab", they don't throw. Designers iterate fast.

#include "ECS/ParticleComponent.h"
#include "ECS/TrailComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/HierarchyComponents.h"

#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace Resource
{

// ---------------------------------------------------------------------------
// VFXEmitterType — the type tag that drives VFXSpawnSystem's dispatch switch.
// Particle/Trail/Beam are component-driven (pure ECS, "Lane A"); the rest are
// resolved by the Renderer via mailbox requests ("Lane B" — GPU pools and
// asset libraries the ECS layer must not depend on directly).
//
// To add a new effect type: append a value here, add a param struct + member
// to VFXEmitterSpec, add a loader directive in VFXPrefab.cpp, and add one
// switch arm in VFXSpawnSystem. Everything else is untouched.
// ---------------------------------------------------------------------------
enum class VFXEmitterType : uint8_t
{
    Particle   = 0,   // ParticleEmitterComponent           (Lane A)
    Trail      = 1,   // TrailComponent                     (Lane A)
    Beam       = 2,   // BeamComponent + MaterialComponent  (Lane A)
    Decal      = 3,   // DecalSpawner via DecalMaterialLibrary (Lane B)
    Tracer     = 4,   // TracerSystem::Spawn                (Lane B)
    Afterimage = 5,   // AfterimageSystem::Spawn on the spawner character (Lane B)
    Mesh       = 6,   // MeshLibrary load → renderable entity (Lane B)

    COUNT
};

// ---- Per-type param blocks. Only the block matching VFXEmitterSpec::type is
//      read at spawn. Beam embeds the real BeamComponent so the spawn is a
//      direct copy; Decal/Tracer/Afterimage/Mesh carry small PODs that the
//      Renderer turns into the right imperative call. -----------------------

struct BeamVFXSpec
{
    BeamComponent     beam;                                          // controlPoints + wobble + radius scale
    // Custom PS for the beam material. Renderer::resolveCustomPSID lazily
    // registers it with the GBuffer + Transparent shader libraries. Path is
    // relative to the working dir (the "shaders/" prefix matters), matching
    // the editor's beam setup (EditorLayer BeamTube_InnerCore/OuterGlow).
    std::string       shaderPath = "shaders/BeamTube_OuterGlow.ps.hlsl";
    bool              additive   = true;                             // true = additive glow, false = opaque core
};

struct DecalVFXSpec
{
    std::string       materialName;                 // resolved via DecalMaterialLibrary::Find on spawn
    float             sizeX           = 0.5f;        // decal width  (world units)
    float             sizeY           = 0.5f;        // decal height (world units)
    float             depth           = 0.2f;        // projection-box thickness
    float             lifetime        = 8.0f;        // seconds; < 0 = static (never expires)
    float             fadeOutDuration = 0.5f;        // seconds of fade before expiry
    float             rollZ           = 0.0f;        // rotation about projection axis (rad)
    DirectX::XMFLOAT4 tintOverride    = { 0, 0, 0, 0 }; // w>0 replaces the asset tint
};

struct TracerVFXSpec
{
    DirectX::XMFLOAT4 color            = { 2.0f, 0.6f, 0.2f, 3.0f }; // linear HDR; .a = intensity
    float             width            = 0.05f;      // beam radius (m)
    float             lifetime         = 0.3f;       // seconds
    float             length           = 8.0f;       // end = origin + dir * length
    DirectX::XMFLOAT3 direction        = { 0, 0, 1 };// local-space fire direction (transformed by spawner pose)
    uint32_t          noiseTexBindless = 0xFFFFFFFFu;// 0xFFFFFFFF = procedural
};

struct AfterimageVFXSpec
{
    float             lifetime = 0.4f;               // seconds; alpha decays 1→0 over this
    DirectX::XMFLOAT4 color    = { 0.4f, 0.7f, 1.0f, 1.0f }; // HDR rgb tint (a ignored)
};

struct MeshVFXSpec
{
    std::string       meshPath;                      // .meshlib — loaded via MeshLibrary by the Renderer (meshId 0)
    std::string       materialPath;                  // .imat — loaded into the instance MaterialComponent
    float             spinDegPerSec = 0.0f;          // optional spin (deg/sec); 0 = static [not yet wired]
    DirectX::XMFLOAT3 spinAxis      = { 0, 1, 0 };
    bool              castShadow    = false;
};

// ---------------------------------------------------------------------------
// VFXEmitterSpec — one emitter entry. The shared header (type, localTransform,
// startDelay, durationOverride) applies to every type; exactly one of the
// per-type param blocks below is consumed based on `type`.
// ---------------------------------------------------------------------------
struct VFXEmitterSpec
{
    // ---- shared header (all types) ----------------------------------------
    VFXEmitterType           type             = VFXEmitterType::Particle;
    // Local-space offset from the spawn root. Composed with the socket / entity
    // transform at instantiation time.
    LocalTransform           localTransform;
    // Per-emitter stagger (sec). 0 = spawn with the rest of the prefab.
    float                    startDelay       = 0.0f;
    // Per-emitter lifetime override (sec). < 0 = inherit the spawn-cmd /
    // prefab duration. Lets a lingering decal outlive a one-shot spark burst.
    float                    durationOverride = -1.0f;

    // ---- per-type param blocks (only the matching one is read) ------------
    ParticleEmitterComponent particle;               // type == Particle
    TrailComponent           trail;                   // type == Trail
    BeamVFXSpec              beam;                    // type == Beam
    DecalVFXSpec             decal;                   // type == Decal
    TracerVFXSpec            tracer;                  // type == Tracer
    AfterimageVFXSpec        afterimage;              // type == Afterimage
    MeshVFXSpec              mesh;                    // type == Mesh
};

struct VFXPrefab
{
    std::vector<VFXEmitterSpec> emitters;
    // Used by VFXSpawnSystem when the caller passes duration <= 0 AND the
    // emitter sets no durationOverride. Negative (or zero) means "no lifetime
    // component" — the emitter runs forever (ambient environmental emitters).
    float                       defaultDuration = 2.0f;
};

// Returns true and fills `out` on success; false (and a logged warning) on
// I/O or parse failure. `out` is always written: a failed load yields an
// empty prefab, which makes VFXSpawnSystem a no-op for that spawn cmd
// instead of crashing the consumer loop.
bool LoadVFXPrefab(const std::string& path, VFXPrefab& out);

} // namespace Resource
