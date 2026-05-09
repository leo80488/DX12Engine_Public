#pragma once

// GlassShatterPass — GPU-driven glass-shatter post-effect.
//
// Lives OUTSIDE the PostProcess::Stack: the stack drives HDR-space compute
// passes whose adapter pattern doesn't fit a one-shot, mostly-idle effect
// like this. Instead the pass runs piggybacked on the same compute command
// list, after Tonemap finishes and before TransitionForDisplay, so it can
// composite directly into the Tonemap output UAV in-place.
//
// CPU API:
//   Init(gfx)       once at startup. Generates a single Voronoi cell pattern
//                    on CPU (Sutherland-Hodgman half-plane clipping), uploads
//                    centroid + edge tables to immutable buffers, compiles the
//                    Init/Simulate/Composite compute shaders.
//   Trigger(u, v)   sets pending-init flag and impact UV. The next Execute()
//                    captures Tonemap output to ShatterSourceTex, dispatches
//                    InitCS to seed per-shard physics, then runs the live
//                    simulate+composite pipeline.
//
// Per-frame flow (only when active):
//   if pending: CopyTextureSubresource(toneMap → shatterSource), dispatch InitCS
//   advance time
//   dispatch SimulateCS
//   dispatch CompositeCS (reads ShatterSource + ShardBuffer + ShardEdges,
//                         writes ToneMap output UAV in-place)
//
// Lifecycle: auto-deactivates once TimeSinceTrigger ≥ Duration. Trigger()
// while active restarts (new impact, new physics).

#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include <DirectXMath.h>
#include <cstdint>

class IGraphicsDevice;

class GlassShatterPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Set the CB params (knobs from EditorLayer / gameplay code).
    void SetImpactStrength(float v)   { m_impactStrength = v; }
    void SetGravityNDC(float v)       { m_gravityNDC = v; }
    void SetAirDrag(float v)          { m_airDrag = v; }
    void SetAngularDamping(float v)   { m_angularDamping = v; }
    void SetCrackWidth(float v)       { m_crackWidth = v; }
    void SetCrackColor(float r, float g, float b);
    void SetDuration(float v)         { m_duration = v; }
    // Seconds the shatter holds in place after Trigger() before shards start
    // moving. During hold the frozen scene is shown with crack lines visible
    // but every shard pinned at translation=0/angle=0. Useful for an impact
    // freeze ("hit-stop") before the breakup.
    void SetHoldDuration(float v)     { m_holdDuration = v; }

    // CPU trigger. Lazily captures the live Tonemap output as the frozen
    // shatter source on the next Execute(). Re-triggering while active
    // restarts everything cleanly.
    void Trigger(float impactU, float impactV);

    bool IsActive() const { return m_active; }

    // Run on a compute command list, AFTER ToneMap finished and BEFORE
    // TransitionForDisplay. `tonemapOutput` is composited in-place when
    // active. `tonemapEntryExitState` is the texture's state on entry; the
    // pass restores that exact state on exit so external state-tracking
    // (ToneMapPass::m_finalOutputState) stays correct.
    void Execute(RHI::CommandList cl,
                 const RHI::Texture* tonemapOutput,
                 RHI::ResourceState tonemapEntryExitState,
                 uint32_t vpW, uint32_t vpH,
                 float deltaTime);

private:
    static constexpr uint32_t kTargetShards = 80;

    // CPU-side mirrors of HLSL structs. Layout MUST match shader-side.
    struct ShardMeta
    {
        DirectX::XMFLOAT2 centroidUV;
        uint32_t          edgeOffset;
        uint32_t          edgeCount;
    };
    struct ShardEdge
    {
        DirectX::XMFLOAT2 normal;
        float             d;
        float             _pad;
    };
    struct ShardGPU
    {
        DirectX::XMFLOAT2 centroidUV;
        DirectX::XMFLOAT2 translation;
        float             angle2D;
        float             angularVel2D;
        DirectX::XMFLOAT2 linearVelocity;
        float             spawnDelay;
        float             alpha;
        float             seed;
        uint32_t          edgeOffset;
        uint32_t          edgeCount;
        float             _pad0;
        float             _pad1;
        float             _pad2;
    };
    struct alignas(16) ShatterCB
    {
        float    TimeSinceTrigger;
        float    DeltaTime;
        float    Duration;
        uint32_t ShardCount;

        DirectX::XMFLOAT2 ImpactPointUV;
        float    ImpactRadialStrength;
        float    GravityNDC;

        float    AirDrag;
        float    AngularDamping;
        float    CrackWidth;
        float    HoldDuration;

        DirectX::XMFLOAT3 CrackColor;
        float    _pad0;

        uint32_t Width;
        uint32_t Height;
        uint32_t _pad1;
        uint32_t _pad2;
    };

    void GenerateVoronoiMesh();
    void EnsureSourceTexture(uint32_t w, uint32_t h);
    void UploadCB(uint32_t w, uint32_t h);

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;

    RHI::PipelineState m_initPSO;
    RHI::PipelineState m_simulatePSO;
    RHI::PipelineState m_compositePSO;

    RHI::GPUBuffer m_metaBuffer;     // SR<ShardMeta>, immutable post-Init
    RHI::GPUBuffer m_edgeBuffer;     // SR<ShardEdge>, immutable post-Init
    RHI::GPUBuffer m_shardBuffer;    // RW<ShardGPU>, GPU-managed lifecycle

    RHI::Texture       m_shatterSource;
    RHI::ResourceState m_shatterSourceState = RHI::ResourceState::COPY_DST;
    uint32_t           m_sourceW = 0, m_sourceH = 0;

    RHI::GPUBuffer m_paramsCB;
    void*          m_paramsCBMapped = nullptr;

    uint32_t m_shardCount = 0;

    // Runtime state.
    bool              m_active            = false;
    bool              m_pendingInit       = false;
    float             m_timeSinceTrigger  = 0.0f;
    DirectX::XMFLOAT2 m_impactUV          { 0.5f, 0.5f };

    // CB knobs.
    float             m_impactStrength    = 0.6f;
    float             m_gravityNDC        = -1.6f;
    float             m_airDrag           = 0.3f;
    float             m_angularDamping    = 0.4f;
    float             m_crackWidth        = 0.0025f;
    DirectX::XMFLOAT3 m_crackColor        { 0.85f, 0.95f, 1.0f };
    float             m_duration          = 3.0f;
    float             m_holdDuration      = 0.6f;  // hit-stop before breakup
};
