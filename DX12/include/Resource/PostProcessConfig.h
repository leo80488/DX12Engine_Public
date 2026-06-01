#pragma once

// PostProcessConfig — a single POD snapshot of every user-tunable
// post-processing parameter exposed in the editor. Persists as a .ippc file
// (line-based text, same formatting conventions as .iscene / .ipfb). A Scene
// asset can reference a .ippc by path so that loading the scene restores its entire
// look (bloom strength, exposure curve, AO parameters, volumetric fog,
// atmosphere / time-of-day, color grading, outline settings, …).
//
// Usage:
//   Save:  PostProcessConfig cfg; cfg.CaptureFrom(renderer); Save(cfg, path);
//   Load:  PostProcessConfig cfg; Load(path, cfg); cfg.ApplyTo(renderer);
//
// Missing keys in the file keep their C++ defaults, so older configs continue
// to work after new fields are added.

#include "RenderGraph/RenderPass/ColorGradingParams.h"
#include <DirectXMath.h>
#include <cstdint>
#include <string>

class Renderer;

namespace Resource
{
    struct PostProcessConfig
    {
        // ---- CASPass -------------------------------------------------------
        bool  casEnabled     = true;
        float casSharpness   = 0.6f;

        // ---- ToneMapPass ---------------------------------------------------
        float              bloomStrength         = 0.04f;
        bool               colorGradingEnabled   = true;
        ColorGradingParams grading;

        // ---- AutoExposurePass ----------------------------------------------
        bool  autoExposureEnabled = true;
        float manualExposure      = 1.0f;   // used when autoExposureEnabled = false
        // Tonemapping response time constant (seconds). Adapter derives per-
        // frame rate as 1 - exp(-dt/tau). Replaces the old adaptationRate
        // field (rate is now derived, not stored).
        float adaptationTau  = 1.5f;
        float minLogLuma     = -5.0f;
        float maxLogLuma     =  3.5f;
        float lowPercent     = 0.50f;
        float highPercent    = 0.85f;
        float minExposure    = 0.10f;
        float maxExposure    = 8.0f;
        float evBias         = 0.0f;
        float keyValue       = 0.18f;

        // ---- XeGTAOPass ----------------------------------------------------
        bool     ssaoEnabled                = true;  // Renderer-level toggle
        float    gtaoEffectRadius           = 0.5f;
        float    gtaoEffectFalloff          = 2.0f;
        float    gtaoFinalValuePower        = 1.5f;
        float    gtaoSampleDistributionPower = 2.0f;
        float    gtaoThinOccluderCompensation = 0.05f;
        float    gtaoDenoiseSigma           = 1.0f;
        uint32_t gtaoSliceCount             = 3;
        uint32_t gtaoStepsPerSlice          = 3;

        // ---- OutlinePass ---------------------------------------------------
        float             outlinePixels     = 2.0f;
        DirectX::XMFLOAT3 outlineColor      { 0.0f, 0.0f, 0.0f };
        float             depthThreshold    = 0.05f;
        float             normalThreshold   = 0.3f;
        float             outlineStrength   = 1.0f;
        float             outlineFadeStart  = 15.0f;
        float             outlineFadeEnd    = 40.0f;

        // ---- VolumetricFogPass --------------------------------------------
        bool              volFogEnabled          = false;
        float             fogDensity             = 0.02f;
        float             fogScattering          = 0.6f;
        float             fogAbsorption          = 0.02f;
        float             fogAnisotropy          = 0.4f;
        float             heightFogStart         = 0.0f;
        float             heightFogFalloff       = 0.05f;
        float             froxelNear             = 0.5f;
        float             froxelFar              = 80.0f;
        DirectX::XMFLOAT3 fogAmbientColor        { 0.4f, 0.55f, 0.85f };
        float             fogAmbientContribution = 0.4f;
        bool              temporalEnabled        = true;
        float             temporalAlpha          = 0.12f;

        // ---- SkyIBLPass (TOD fields moved to TODConfigComponent in ECS) ----
        bool              atmosphereEnabled      = false;
        uint32_t          skyboxSource           = 0;    // 0 = Atmosphere, 1 = Static
        float             iblStrength            = 0.1f;
        bool              aerialCompositeEnabled = false;

        // Populate this struct from the renderer's current live parameters.
        // Skips passes that don't exist (e.g. before first Init).
        void CaptureFrom(const Renderer& r);

        // Push this struct's values onto the renderer's passes. Missing passes
        // are silently skipped.
        void ApplyTo(Renderer& r) const;
    };

    // Serialize a PostProcessConfig to `path` (.ippc text format).
    bool SavePostProcessConfig(const PostProcessConfig& cfg, const std::string& path);

    // Parse a .ippc file into `out`. Missing keys keep their defaults. Returns
    // false only on I/O failure — corrupt/missing fields are tolerated.
    bool LoadPostProcessConfig(const std::string& path, PostProcessConfig& out);
}
