#pragma once

// IPostProcessEffect — Phase 1 scaffolding for the PostProcess::Stack.
//
// Minimum interface that lets Renderer drive the post-process chain through
// a unified `Stack::Execute()` call instead of a hardcoded sequence of pass
// pointers. Future phases extend this with DeclareResources (RT pool),
// GetParameterBlockSize (parameter store), etc.
//
// Concurrency: Execute() runs on the compute render worker (same context as
// the legacy pass->Execute calls it replaces). Do NOT capture the Context
// beyond a single Execute call.

#include <cstdint>
#include <DirectXMath.h>
#include "Graphics/GraphicsStruct.h"  // RHI::CommandList

class IGraphicsDevice;
class World;

namespace PostProcess
{

class ParameterStore;

// Fixed execution slots. Order = execution order.
//
// Reserved slots are placeholders so later phases can plug effects in without
// renumbering. Only slots marked `current` are populated in Phase 1; the
// others are no-ops until we add the corresponding effect.
enum class Stage : uint8_t
{
    // ---- HDR-space ----
    DepthOfField,          // reserved
    MotionBlur,            // reserved
    CAS,                   // current: AMD FidelityFX sharpen (pre-exposure)
    AutoExposure,          // current: histogram-based EV
    Bloom,                 // current: Sledgehammer bloom chain
    LensFlare,             // current: procedural directional-light lens flare
    ChromaticAberration,   // reserved
    LensDistortion,        // reserved

    // ---- HDR → LDR crossing ----
    Tonemapping,           // current: ACES + baked 3D LUT color grading

    // ---- LDR-space ----
    ColorGrading,          // reserved (currently folded into Tonemapping)
    Vignette,              // reserved
    FilmGrain,             // reserved
    Custom,                // reserved for scripted / volume-driven effects
    AntiAliasing,          // reserved (FXAA/SMAA — TAA runs outside the stack)

    Count
};

// Per-frame bus threaded through every effect. Acts as the "ping-pong
// pointer" in Phase 1: effects that consume the running HDR result read
// `hdrSrv`; effects that produce a new one overwrite it. Side-band fields
// (`bloomSrv`, `exposureSrv`) decouple producers from consumers that live
// several stages apart.
struct Context
{
    RHI::CommandList cl{};
    IGraphicsDevice* gfx = nullptr;

    uint32_t viewportWidth  = 0;
    uint32_t viewportHeight = 0;
    float    deltaTime      = 0.0f;

    // World-space camera position. Fed to VolumeSystem for per-frame
    // weight computation (distance-to-boundary falloff).
    DirectX::XMFLOAT3 cameraPos = { 0.0f, 0.0f, 0.0f };

    // ECS world — read by EntityVolumeSource to iterate VolumeComponent.
    // Null when called outside a scene context. Non-owning.
    class World* world = nullptr;

    // Running HDR scene SRV. Mutated by HDR-space producer effects (CAS, TAA
    // upstream). Readers: AutoExposure, Bloom, Tonemapping.
    uint64_t hdrSrv = 0;

    // Written by AutoExposure effect, read by Tonemapping effect.
    uint64_t exposureSrv = 0;
    // Written by Bloom effect, read by Tonemapping effect.
    uint64_t bloomSrv = 0;
    // Written by LensFlare effect, read by Tonemapping effect.
    uint64_t lensFlareSrv = 0;

    // Authoritative parameters for every Stage. Adapters read their block
    // here; EditorLayer / config / future Volume blender write to it.
    // Non-owning — lifetime is the PostProcess::Stack that produced the
    // context.
    ParameterStore* params = nullptr;
};

class IEffect
{
public:
    virtual ~IEffect() = default;

    virtual Stage       GetStage() const = 0;
    virtual const char* GetName()  const = 0;

    // Zero-cost skip gate. Returns false when the effect shouldn't run this
    // frame (disabled, weight == 0, inputs unavailable). Stack records no
    // commands and leaves Context untouched for false returns.
    virtual bool IsEnabled(const Context& ctx) const = 0;

    // Record compute commands onto ctx.cl and (if the effect produces output)
    // mutate the relevant Context SRV slot for downstream readers.
    virtual void Execute(Context& ctx) = 0;
};

} // namespace PostProcess
