#pragma once

// DDGIComponents.h — ECS data for Dynamic Diffuse Global Illumination.
//
// Three component types + one scene-singleton settings struct:
//
//   DDGIVolumeComponent          : authored params (origin/extent/probe counts/...)
//                                  Serializable. Editor-edited.
//   DDGIVolumeRuntimeComponent   : GPU resource handles owned by DDGIVolumeManager.
//                                  Not serialized; rebuilt on volume create/resize.
//   DDGISceneTagComponent        : marker — entities tagged here join the DDGI TLAS
//                                  (static geometry only — skinned meshes are excluded
//                                  by the system itself even if mistakenly tagged).
//   IndirectLightingSettings     : engine-global integration knobs. Lives at scene
//                                  scope (one entity per world); also exposed via
//                                  Renderer for game/editor code to mutate.

#include "Resource/SystemHandles.h" // ResourceHandle uint64_t typedef
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

// =============================================================================
// DDGIVolumeComponent — authored parameters
// =============================================================================
//
// One component per DDGI volume in the scene. Volumes are arbitrarily-positioned
// AABBs filled with a regular grid of probes. Multiple volumes may overlap;
// shading uses a weighted blend (priority + boundary fade) to pick the winner.
//
// Default values match the plan §3.1 reference (interior room, 16x8x16 probes,
// 64 rays/probe). Larger scenes / more rays trade quality for cost.

struct DDGIVolumeComponent
{
    DirectX::XMFLOAT3 origin       = { 0.0f, 0.0f, 0.0f };  // world-space center
    DirectX::XMFLOAT3 extent       = { 5.0f, 3.0f, 5.0f };  // half-size of the AABB

    // Probe grid resolution per axis. Practical bounds: each component in [4, 32].
    // Total probes = X * Y * Z. Atlas size scales linearly with this.
    uint32_t          probeCountsX = 16;
    uint32_t          probeCountsY = 8;
    uint32_t          probeCountsZ = 16;

    // Rays per probe per frame. Common values: 64 (perf), 128 (default), 256 (quality).
    uint32_t          raysPerProbe = 128;

    // EMA hysteresis for the per-texel relight blend.
    //   blendSpeed = 1 - hysteresis    (per-frame contribution of new samples)
    //   0.92 → 8 %/frame  → ~0.5 s to 90 % at 60 fps  (default — responsive)
    //   0.965 → 3.5 %/frame → ~1.1 s to 90 % at 60 fps  (smooth, slow)
    //   0.85 → 15 %/frame → noisier but tracks fast motion
    float             hysteresis   = 0.92f;
    // Sampling biases (world units). Plan §5.3.
    float             normalBias   = 0.25f;
    float             viewBias     = 0.10f;
    // Volume-edge fade ratio (0..0.5). Smooths the transition at AABB boundaries.
    float             boundaryFadeRatio = 0.12f;

    // Toggles for advanced features (Phase 3+). Phase 1 ignores these — kept on
    // the component so editors can tee them up without breaking serialization.
    bool              enableRelocation     = true;
    bool              enableClassification = true;

    // Volume priority — higher wins ties when probes overlap. Cascade level
    // hints which volume sits "in front" in multi-cascade configurations
    // (Phase 5). 0 = nearest cascade.
    int               priority      = 0;
    uint32_t          cascadeLevel  = 0;

    // Editor-only: visible toggle for the DDGI debug viewer. Has no shading effect.
    bool              debugDraw     = false;

    // Per-volume tint for diffuse (mostly an art-side debug knob; default 1.0
    // leaves DDGI fully physical).
    DirectX::XMFLOAT3 diffuseTint   = { 1.0f, 1.0f, 1.0f };
    float             diffuseScale  = 1.0f;
};

// =============================================================================
// DDGIVolumeRuntimeComponent — GPU resource handles
// =============================================================================
//
// Maintained by DDGIVolumeManager: created when a DDGIVolumeComponent is added
// to an entity, resized when probeCounts/raysPerProbe change, freed on remove.
// Components hold opaque uint64_t handles into the manager's resource pool.

using DDGIResourceHandle = uint64_t;
inline constexpr DDGIResourceHandle kInvalidDDGIResource = 0xFFFFFFFFFFFFFFFFull;

struct DDGIVolumeRuntimeComponent
{
    // Index into DDGIVolumeManager::m_volumes. ~0u = not yet allocated.
    uint32_t           volumeSlot = 0xFFFFFFFFu;

    // Snapshot of the param-derived sizes. Manager compares against current
    // DDGIVolumeComponent values to detect resize requests (probeCounts /
    // raysPerProbe changed) — equality skips realloc for static volumes.
    uint32_t           probeCountX = 0;
    uint32_t           probeCountY = 0;
    uint32_t           probeCountZ = 0;
    uint32_t           raysPerProbe = 0;

    // Per-volume frame counter — used by the relight shader to pick a
    // deterministic ray rotation seed per frame. Ticks once per DDGI update.
    uint32_t           frameCounter = 0;

    // Frames left of "atlas overwrite mode" — when > 0 the relight CS skips
    // the hysteresis lerp and writes the fresh sample directly. Lets a newly-
    // allocated volume converge in 1-2 frames regardless of the GPU memory
    // state the atlas was created in. Decremented by DDGIVolumeManager::Tick.
    uint32_t           firstFramesRemaining = 3;

    // Per-frame random rotation matrix (3x3 packed as 9 floats; row-major).
    // Computed in DDGIVolumeUpdateSystem from a low-discrepancy sequence.
    float              randomRotation[9] = { 1,0,0, 0,1,0, 0,0,1 };

    // Toroidal scroll offset (Phase 5). Phase 1 keeps this at zero — volumes
    // are static in world space.
    int32_t            scrollOffsetX = 0;
    int32_t            scrollOffsetY = 0;
    int32_t            scrollOffsetZ = 0;
};

// =============================================================================
// DDGISceneTagComponent — TLAS membership marker
// =============================================================================
//
// Tag entities (only static-geometry entities) that should contribute to the
// DDGI TLAS. The TLAS build system iterates the union of (a) explicit tags
// and (b) auto-included entities (any non-skinned MeshHandle inside a volume's
// influence AABB). Adding the tag forces inclusion regardless of (b).

struct DDGISceneTagComponent
{
    // Reserved for future per-entity tweaks (e.g. local IBL fallback color).
    // Empty in Phase 1 — the tag's mere presence is the meaningful state.
    uint8_t reserved = 0;
};

// =============================================================================
// IndirectLightingSettingsComponent — scene-singleton integration knobs
// =============================================================================
//
// Lives on a single "settings" entity per world. Drives the fallback chain in
// Lighting.ps.hlsl. Game / editor code mutates these through Renderer's
// GetIndirectLightingSettings() accessor.

struct IndirectLightingSettingsComponent
{
    // Master DDGI on/off — when false the Lighting shader skips DDGI sampling
    // entirely and falls back to Sky IBL diffuse for indirect.
    bool   ddgiEnabled               = true;

    // Global scale on DDGI diffuse contribution. NPR skin materials should
    // use a per-material override (0.3..0.5) — this knob is the engine-wide one.
    float  ddgiDiffuseScale          = 1.0f;

    // Sky IBL diffuse scale (used as fallback when no DDGI volume covers the
    // shading point). Acts as a global ambient cap.
    float  skyIBLDiffuseScale        = 1.0f;

    // AO attenuation on the DDGI diffuse path. DDGI probes already encode
    // mid-scale visibility (probe-spacing distances ~0.5-2 m); re-applying
    // full screen-space AO on top would darken those scales twice. This
    // factor lerps the AO term toward 1.0 only for the DDGI contribution —
    // 0.0 disables AO on DDGI entirely, 1.0 keeps the legacy double-occlusion
    // behaviour. ~0.4 retains near-field contact shadow without re-doing
    // mid-scale occlusion. The Sky-IBL fallback path always uses full AO.
    float  ddgiAONearFieldStrength   = 0.4f;

    // SSR tuning — already used by the existing SSR composite, but exposed
    // here for the integration to stay coherent. Editor binds the same fields.
    bool   ssrEnabled                = true;
    float  ssrRoughnessCutoff        = 0.6f;   // smoothstep(cutoff-0.1, cutoff+0.1, roughness)
    float  ssrEdgeFadeRatio          = 0.10f;  // screen-edge fade band

    // Whether reflection probes win over DDGI for rough specular fallback.
    // When false, very-rough surfaces (roughness > 0.7) inside a DDGI volume
    // sample the probe diffuse atlas as a poor-man's specular fallback to fill
    // gaps where neither SSR nor a probe covers them. See plan §5.1 Layer D.
    bool   reflectionProbePriorityOverDDGI = true;
    bool   useDDGIForRoughSpecularFallback = false;
};
