#pragma once

// XeGTAOPass — Screen-space Ground Truth Ambient Occlusion (compute).
//
// Runs in the post-processing compute phase (COMPUTE queue, alongside TAA/Bloom).
// Result is consumed by LightingPass on the NEXT frame (one-frame latency,
// standard practice — invisible with TAA).
//
// Two dispatches per frame:
//   1. GTAO Main  — horizon-based AO from depth + normals → raw AO (R8_UNORM)
//   2. Denoise    — edge-aware bilateral blur → final AO (R8_UNORM)

#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include <DirectXMath.h>

class IGraphicsDevice;

class XeGTAOPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Per-frame setters (called by Renderer before Worker 2 kicks).
    void SetDepthSrvHandle(uint64_t h)    { m_depthSrvHandle    = h; }
    void SetNormalSrvHandle(uint64_t h)   { m_normalSrvHandle   = h; }
    void SetVelocitySrvHandle(uint64_t h) { m_velocitySrvHandle = h; }
    void SetViewportSize(uint32_t w, uint32_t h);
    // Pass the engine's projection + view matrices (row-major XMFLOAT4X4 from
    // XMMatrixPerspectiveFovLH / LookToLH — reversed-Z or standard-Z both
    // work, the helper auto-detects handedness from the sign of the Z
    // coefficients). ViewMatrix is used to transform GBuffer world normals
    // into view space for the GTAO integration.
    void SetProjectionMatrix(const DirectX::XMFLOAT4X4& projMatrix,
                             const DirectX::XMFLOAT4X4& viewMatrix,
                             uint32_t frameCounter);

    // Record compute dispatches onto cl (COMPUTE or GRAPHICS CL).
    void Execute(RHI::CommandList cl);

    // Returns the SRV handle of the final denoised AO texture.
    // Valid after at least one Execute() call. Used by LightingPass next frame.
    uint64_t GetAOSrvHandle() const;

    // Tuning (public for editor UI if desired). Defaults mirror Intel's
    // reference `GTAOSettings` with QualityLevel=2 (HIGH preset):
    //   LOW    = 1 × 2 samples; MEDIUM = 2 × 2; HIGH = 3 × 3; ULTRA = 9 × 3.
    // Values copied from XeGTAO.h XE_GTAO_DEFAULT_* constants.
    float    effectRadius              = 0.5f;
    float    radiusMultiplier          = 1.457f;   // XE_GTAO_DEFAULT_RADIUS_MULTIPLIER
    float    effectFalloffRange        = 0.615f;   // XE_GTAO_DEFAULT_FALLOFF_RANGE
    float    sampleDistributionPower   = 2.0f;     // XE_GTAO_DEFAULT_SAMPLE_DISTRIBUTION_POWER
    float    thinOccluderCompensation  = 0.0f;     // XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION
    float    finalValuePower           = 2.2f;     // XE_GTAO_DEFAULT_FINAL_VALUE_POWER
    float    depthMIPSamplingOffset    = 3.30f;    // XE_GTAO_DEFAULT_DEPTH_MIP_SAMPLING_OFFSET
    float    denoiseBlurBeta           = 1.2f;     // per-ref, when denoise on
    uint32_t sliceCount                = 3;        // HIGH preset
    uint32_t stepsPerSlice             = 3;

    // Temporal accumulation — runs AFTER the spatial denoise (pass order:
    // Main → Denoise → Temporal). Because the input is a denoised, low-σ
    // signal rather than raw 1-spp blue-noise AO, the variance clip in the
    // shader becomes a real disocclusion test and we can push α higher
    // without re-introducing the crevice ghosts that plagued the previous
    // Main → Temporal → Denoise order (where the wide raw-AO σ swallowed
    // every stale history value as "plausible").
    float    temporalHistoryAlpha      = 0.95f;    // 0 disables accumulation
    // Disocclusion threshold applied to clampDelta = |history − variance-clipped
    // history|. With denoised input σ collapses, the clip window is tight, and
    // clampDelta is effectively zero on converged surfaces. 0.05 rejects
    // history that was clipped by more than 5% of the AO range — fast enough
    // to kill trailing ghosts in 1 frame without harming steady-state
    // accumulation. (Old 0.15 default was tuned for raw-AO variance.)
    float    temporalRejectionDiff     = 0.05f;

private:
    void RebuildTextures();

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;

    RHI::PipelineState m_mainPSO;
    RHI::PipelineState m_temporalPSO;   // AO-only temporal accumulation
    RHI::PipelineState m_denoisePSO;
    RHI::PipelineState m_prefilterPSO;  // 16x16 groupshared 5-mip prefilter

    // Persistent constant buffer — partitioned 8 × 256-byte slots, indexed by
    // dispatch type (see kCBOffset* in .cpp).
    static constexpr uint32_t kCBTotalSize = 256 * 8;
    struct alignas(256) GTAOCBPool { uint8_t bytes[kCBTotalSize]; };
    FrameCB<GTAOCBPool> m_cb;

    // 5-mip linear depth pyramid — written by the prefilter's single dispatch
    // (mips 0..4 bound as u0..u4 space2). Main pass samples the pyramid with
    // SampleLevel + a log2-based mip selection (reference's distant-sample
    // MIP optimization).
    static constexpr uint32_t kDepthMipLevels = 5;
    RHI::Texture       m_linearDepth;
    RHI::ResourceState m_linearDepthState = RHI::ResourceState::UNORDERED_ACCESS;

    // AO textures: raw (main pass output) and final (denoise output).
    RHI::Texture       m_aoRaw;
    RHI::Texture       m_aoFinal;
    RHI::Texture       m_edges;               // packed 4-edge mask (R8_UNORM)
    RHI::ResourceState m_aoRawState   = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_aoFinalState = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_edgesState   = RHI::ResourceState::UNORDERED_ACCESS;

    // 64x64 R16_UINT Hilbert-curve lookup. Index = HilbertIndex(x, y) for the 6-level
    // Hilbert curve (XE_HILBERT_LEVEL = 6, Width = 64, Area = 4096). SpatioTemporalNoise
    // reads this + applies R2 to produce a decorrelated blue-noise-like 2D value per
    // pixel/frame. Lifetime = pass lifetime (data never changes).
    RHI::Texture       m_hilbertLUT;
    uint64_t           m_hilbertLUTSrv = 0;

    // 1x1 zero-velocity fallback bound to the velocity slot (t4 space2, root 8)
    // whenever no GBuffer velocity SRV is available. XeGTAOTemporal.cs reads
    // gVelocity unconditionally, so the slot must never be left unbound or
    // GPU-Based Validation reports "uninitialized root argument accessed" every
    // frame. Zero velocity == no reprojection (the intended "no history" case).
    RHI::Texture       m_zeroVelocityTex;
    uint64_t           m_zeroVelocitySrv = 0;

    // AO-only temporal history — ping-pong between two buffers. Each frame
    // reads from readIdx (last frame's write), writes to writeIdx. After
    // Execute, writeIdx is the fresh accumulated AO; it feeds the spatial
    // denoise AND next frame's history read.
    RHI::Texture       m_aoHistory[2];
    RHI::ResourceState m_aoHistoryState[2] = { RHI::ResourceState::UNORDERED_ACCESS,
                                                RHI::ResourceState::UNORDERED_ACCESS };

    // Prev-frame linear-depth ping-pong. R32F at mip-0 resolution, indexed
    // with the same readIdx/writeIdx as m_aoHistory so the depth that
    // accompanied a history AO value is the depth at the SAME slot.
    // Temporal pass uses it for a plane-distance disocclusion rejection: if
    // depth at the reprojected UV disagrees with current-frame depth at the
    // pixel, history is coming from a different surface (classic
    // dynamic-object reveal case — hand moves and exposes background, or an
    // object's silhouette slides across the camera). α collapses toward 0
    // there and the fresh denoised sample wins, so the ghost dies in one
    // frame instead of bleeding for 10-20.
    RHI::Texture       m_prevLinearDepth[2];
    RHI::ResourceState m_prevLinearDepthState[2] = { RHI::ResourceState::UNORDERED_ACCESS,
                                                      RHI::ResourceState::UNORDERED_ACCESS };

    uint32_t           m_historyWriteIdx = 0;   // slot next Execute will write into
    uint32_t           m_historyFreshIdx = 0;   // slot holding the JUST-written AO;
                                                 //   what GetAOSrvHandle returns.
                                                 //   Equal to the previous Execute's
                                                 //   writeIdx (set at end of Execute).
    bool               m_historyValid    = false;   // false until after first Execute
    // External hard-cut signal driven by the camera-stack pipeline (LiveCamera.
    // historyValid). When false this frame, temporal sampling falls through to
    // the "no history" branch even if m_historyValid is true. AND'd with the
    // internal flag so resize / first-frame / explicit cut all funnel through
    // one gate in the shader CB fill.
    bool               m_externalHistoryValid = true;

public:
    void SetExternalHistoryValid(bool b) { m_externalHistoryValid = b; }
private:

    // Per-frame inputs (set by Renderer).
    uint64_t m_depthSrvHandle    = 0;
    uint64_t m_normalSrvHandle   = 0;
    uint64_t m_velocitySrvHandle = 0;
    uint32_t m_vpW = 0, m_vpH = 0;
    bool     m_texDirty = true;

    // Cached projection/view matrices + frame counter for CB fill.
    DirectX::XMFLOAT4X4 m_projMatrix{};
    DirectX::XMFLOAT4X4 m_viewMatrix{};
    uint32_t            m_frameCounter = 0;

    // GTAOConstants — VERBATIM layout from Intel XeGTAO.h. Must match the
    // cbuffer in XeGTAO.hlsli exactly. Do not rearrange fields.
    struct alignas(16) GTAOConstants
    {
        int32_t  ViewportSize[2];
        float    ViewportPixelSize[2];

        float    DepthUnpackConsts[2];
        float    CameraTanHalfFOV[2];

        float    NDCToViewMul[2];
        float    NDCToViewAdd[2];

        float    NDCToViewMul_x_PixelSize[2];
        float    EffectRadius;
        float    EffectFalloffRange;

        float    RadiusMultiplier;
        float    Padding0;
        float    FinalValuePower;
        float    DenoiseBlurBeta;

        float    SampleDistributionPower;
        float    ThinOccluderCompensation;
        float    DepthMIPSamplingOffset;
        int32_t  NoiseIndex;

        // Engine extension — a few extra fields that we need (sliceCount /
        // stepsPerSlice are dispatched via dynamic values in the reference
        // via HLSL template params; we just pass them in the CB).
        uint32_t SliceCount;
        uint32_t StepsPerSlice;
        float    _pad1[2];

        // Engine extension — world→view matrix for GBuffer normal transform.
        float    ViewMatrix[16];
    };
    static_assert(sizeof(GTAOConstants) % 16 == 0, "GTAOConstants must be 16-byte aligned");

    struct alignas(16) DenoiseCB
    {
        uint32_t viewportWidth;
        uint32_t viewportHeight;
        float    depthLinearizeMul;
        float    depthLinearizeAdd;
        float    denoiseBlurBeta;
        uint32_t finalApply;          // 0 = intermediate pass; 1 = final pass
        float    _pad[2];
    };

    struct alignas(16) TemporalCB
    {
        uint32_t viewportWidth;
        uint32_t viewportHeight;
        float    historyAlpha;        // 0 = first frame / accumulation disabled
        float    rejectionDiff;       // |curr-hist| threshold for smooth rejection
        float    _pad[4];
    };
};
