#pragma once

// SkyIBLPass — projects the current environment cubemap into L2 spherical
// harmonics each frame. Consumed by LightingPass for a dynamic diffuse IBL
// term that replaces the old static cubemap irradiance sample.
//
// This is Phase 1 of the atmospheric PBR IBL plan (see
// dx12_pbr_atmospheric_ibl_prompt.md). Phase 2 (async specular ping-pong)
// and Phase 3 (atmosphere LUTs) will extend this pass with additional
// resources and compute dispatches.
//
// Resource boundary:
//   - Owns one UAV/SRV-visible structured buffer (9 × float4) that holds the
//     SH coefficients produced by SkySHProjection.cs.hlsl.
//   - Does NOT own the source environment cubemap — Renderer sets its GPU
//     handle each frame via SetSourceCubemap(). When the handle is 0, the
//     pass is a no-op and the SH buffer remains as last computed (or zero).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

#include <array>
#include <cmath>
#include <DirectXMath.h>

class SkyIBLPass : public RG::RenderPass
{
public:
    SkyIBLPass() = default;

    const char* GetName() const override { return "SkyIBLPass"; }
    void Setup  (RG::RenderGraphBuilder& b)     override
    {
        // Compute-only pass — no render target, no declared reads/writes on
        // graph-managed textures. The source cubemap handle is set externally.
        b.SetColorTarget(RG::BuiltinTexture::None);
    }
    void Init   (IGraphicsDevice& gfx)          override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    /** Set the source environment cubemap SRV GPU handle. Pass 0 to skip. */
    void SetSourceCubemap(uint64_t gpuHandle) { m_srcCubemap = gpuHandle; }

    /** Returns the GPU SRV handle for the SH coefficient buffer (9 × float4).
     *  Used by LightingPass to bind the SH buffer at its shader slot. */
    uint64_t GetSHSrvHandle() const { return m_shSrvHandle; }

    /** True once at least one SH projection has produced valid coefficients. */
    bool IsSHValid() const { return m_shValid; }

    /** GPU SRV handle for the pre-filtered specular cubemap (7 mips).
     *  Returns 0 until the first prefilter completes. */
    uint64_t GetSpecularSrvHandle() const { return m_specSrvHandle; }

    /** True once the specular cubemap has been pre-filtered at least once. */
    bool IsSpecularValid() const { return m_specValid; }

    /** Mip count of the pre-filtered specular cubemap (fixed at 7 for 128×128). */
    uint32_t GetSpecularMipCount() const { return kSpecularMips; }

    /** Per-face resolution of mip 0 of the pre-filtered specular cubemap. */
    static constexpr uint32_t GetSpecularSize() { return kSpecularSize; }

    /** Raw specular texture handle for external copy operations
     *  (e.g. broadcasting into the reflection-probe cubemap-array). The caller
     *  must respect the texture's tracked state via GetSpecularState() and
     *  apply matching barriers around any copy or transition it issues. */
    const RHI::Texture* GetSpecularTexture() const { return &m_specTex; }
    RHI::ResourceState  GetSpecularState()   const { return m_specState; }
    void                SetSpecularState(RHI::ResourceState s) { m_specState = s; }

    /** Enable the procedural atmospheric sky (Phase 3). When enabled, the
     *  internal atmosphere cubemap drives SH projection + specular pre-filter.
     *  When disabled, all per-frame compute in SkyIBLPass::Execute() is
     *  skipped and the lighting path falls back to the static irradiance +
     *  radiance cubemaps loaded from disk. Re-enabling schedules a full
     *  6-face pre-filter re-bake so the cube is valid immediately. */
    void SetAtmosphereEnabled(bool on)
    {
        if (on && !m_atmosphereEnabled)
            m_prefilterNeedsFullBake = true;
        m_atmosphereEnabled = on;
    }
    bool IsAtmosphereEnabled() const   { return m_atmosphereEnabled; }

    /** Skybox backdrop source — atmosphere = use procedural sky cubemap,
     *  Static = use the externally-loaded .itex handle (SetSourceCubemap).
     *  Defaults to Atmosphere when atmosphere is enabled. */
    enum class SkyboxSource : uint8_t { Atmosphere = 0, Static = 1 };
    void SetSkyboxSource(SkyboxSource s) { m_skyboxSource = s; }
    SkyboxSource GetSkyboxSource() const { return m_skyboxSource; }

    /** Resolve the correct cubemap SRV for SkyboxPass based on the current
     *  source selection. Returns 0 if neither source is ready. */
    uint64_t ResolveSkyboxSrvHandle(uint64_t staticFallback) const;

    /** Update the sun direction / radiance used by the atmosphere shader.
     *  `dir` points FROM ground TOWARDS the sun (unit), `color` is linear RGB.
     *  When the time-of-day controller is enabled, this call is overridden
     *  by the time-based sun direction — set the time instead. */
    void SetSunDir(const DirectX::XMFLOAT3& dir, const DirectX::XMFLOAT3& color)
    {
        m_sunDir = dir;
        m_sunColor = color;
    }

    // ---- Time-of-day controller -----------------------------------------------
    /** Enable the built-in day/night cycle. When on, the sun direction is
     *  computed from timeOfDay and overrides the scene's directional light. */
    void SetTimeOfDayEnabled(bool on) { m_timeOfDayEnabled = on; }
    bool IsTimeOfDayEnabled() const   { return m_timeOfDayEnabled; }

    /** 0 = midnight, 0.25 = sunrise, 0.5 = noon, 0.75 = sunset. Wraps modulo 1. */
    void SetTimeOfDay(float t)        { m_timeOfDay = t - std::floor(t); }
    float GetTimeOfDay() const        { return m_timeOfDay; }

    /** Time advance rate in "day units" per real second. 1.0 = 1 full day
     *  per second (very fast, useful for demoing); set 1/60.0 for 1-minute days;
     *  0 pauses. */
    void SetTimeSpeed(float unitsPerSec) { m_timeSpeed = unitsPerSec; }
    float GetTimeSpeed() const           { return m_timeSpeed; }

    /** Geographic latitude in radians — controls how high the sun climbs.
     *  0 = equator (sun passes zenith), ±π/2 = poles. Default mid-latitude. */
    void SetLatitude(float rad)          { m_latitudeRad = rad; }
    float GetLatitude() const            { return m_latitudeRad; }

    /** Master sun intensity multiplier. Scales m_sunColor BEFORE it's uploaded
     *  to the atmosphere / SH / specular / LightCB consumers. Use this as the
     *  single knob to dim/brighten the whole sky + IBL + direct lighting when
     *  the scene feels too bright or too dark. Default 1.0. */
    void SetSunIntensityScale(float s) { m_sunIntensityScale = s; }
    float GetSunIntensityScale() const { return m_sunIntensityScale; }

    /** Global IBL strength. Scales diffuse + specular IBL contribution in
     *  Lighting.ps.  Was previously read from SkyboxComponent per-scene;
     *  now a single global knob (overrides any SkyboxComponent value). */
    void SetIBLStrength(float s)   { m_iblStrength = s; }
    float GetIBLStrength() const   { return m_iblStrength; }

    /** Flat ambient base colour (written into LightCB::ambient). The lighting
     *  shader multiplies this by albedo and adds unconditionally as a small
     *  "fill" term independent of IBL strength. Tune down to black when you
     *  want IBL to be the only indirect contribution. */
    void SetAmbientColor(const DirectX::XMFLOAT3& c) { m_ambientColor = c; }
    const DirectX::XMFLOAT3& GetAmbientColor() const { return m_ambientColor; }

    /** Advance the time-of-day by `deltaSeconds` if enabled. Computes the
     *  resulting sun direction/color and updates internal state. Call once
     *  per frame from Renderer. Returns true if sun dir changed. */
    bool TickTimeOfDay(float deltaSeconds);

    /** Current computed sun direction (unit, towards sun). Valid after Tick.
     *  When the moon is the active body (sun below horizon), this returns the
     *  MOON direction (= -solarDir) so all downstream lighting / shadow code
     *  automatically follows the moon without any special-casing. */
    const DirectX::XMFLOAT3& GetSunDir()   const { return m_sunDir; }
    const DirectX::XMFLOAT3& GetSunColor() const { return m_sunColor; }

    /** True when the night-time path is active (sun is below horizon and the
     *  moon has taken over as the directional light source). SkyboxPass uses
     *  this to switch the analytic disk to a textured moon. */
    bool  IsMoonActive() const { return m_isMoon; }

    /** Moon disk direction + colour + visibility. These are tracked separately
     *  from the "active body" pair so the skybox can keep drawing the moon
     *  while it descends below horizon (and start drawing it before moonrise
     *  flips the active body), without the active-body lighting switch having
     *  to overlap across the horizon. */
    const DirectX::XMFLOAT3& GetMoonDir()   const { return m_moonDir; }
    const DirectX::XMFLOAT3& GetMoonColor() const { return m_moonColor; }
    bool                     IsMoonDiskVisible() const { return m_moonDiskVisible; }

    /** Tunable moon brightness multiplier (relative to sun's basePeak). */
    void  SetMoonIntensityScale(float s) { m_moonIntensityScale = s; }
    float GetMoonIntensityScale() const  { return m_moonIntensityScale; }

    /** SRV of the procedural sky cubemap (for SkyboxPass to sample as backdrop).
     *  Returns 0 if atmosphere has never run. */
    uint64_t GetAtmosphereSrvHandle() const { return m_atmosphereSrvHandle; }

    /** SRV of the 3D Aerial Perspective LUT (32×32×32). Consumed by the
     *  lighting pass to fade distant opaque geometry into atmospheric fog.
     *  Returns 0 before the first atmosphere dispatch. */
    uint64_t GetAerialPerspectiveSrvHandle() const { return m_aerialSrvHandle; }
    float    GetAerialMaxDistanceKm() const        { return kAerialMaxKm; }

    /** True after the first successful AP dispatch. Before this the 3D LUT
     *  holds zero/garbage, so the lighting pass should NOT composite it
     *  (would multiply opaque pixels by ap.a = 0 → black screen). */
    bool IsAerialValid() const { return m_aerialValid; }

    /** Enable / disable the Aerial Perspective composite in the lighting pass.
     *  Defaults to OFF — atmosphere, SH, specular and skybox all still work,
     *  only the distance-fog composite is skipped. Useful when debugging
     *  a scene whose engine units don't match the shader's km assumption. */
    void SetAerialCompositeEnabled(bool on) { m_aerialCompositeEnabled = on; }
    bool IsAerialCompositeEnabled() const   { return m_aerialCompositeEnabled; }

    /** Per-frame camera state required by the Aerial Perspective LUT. Uses
     *  the same `invViewProj` convention as LightCB (row-vector matrix). */
    void SetCameraForAerial(const DirectX::XMFLOAT3& cameraPosWorld,
                            const DirectX::XMFLOAT3& cameraForwardWorld,
                            const float invViewProjRowMajor[16])
    {
        m_cameraPosWorld     = cameraPosWorld;
        m_cameraForwardWorld = cameraForwardWorld;
        for (int i = 0; i < 16; ++i) m_invViewProj[i] = invViewProjRowMajor[i];
    }

private:
    static constexpr uint32_t kSpecularSize       = 128;
    static constexpr uint32_t kSpecularMips       = 7; // log2(128) + 1
    static constexpr uint32_t kAtmosphereSize     = 256;

    // Hillaire 2020 LUT sizes.
    static constexpr uint32_t kTransmittanceW = 256;
    static constexpr uint32_t kTransmittanceH = 64;
    static constexpr uint32_t kMultiScatterW  = 32;
    static constexpr uint32_t kMultiScatterH  = 32;
    static constexpr uint32_t kSkyViewW       = 192;
    static constexpr uint32_t kSkyViewH       = 108;
    static constexpr uint32_t kAerialW        = 32;
    static constexpr uint32_t kAerialH        = 32;
    static constexpr uint32_t kAerialD        = 32;
    static constexpr float    kAerialMaxKm    = 32.0f; // far slice = 32 km

    void CreateSpecularCube(IGraphicsDevice& gfx);
    void CreateAtmosphereCube(IGraphicsDevice& gfx);
    void CreateAtmosphereLUTs(IGraphicsDevice& gfx);
    void BakeStaticLUTs(RHI::CommandList cl);   // Transmittance + MultiScatter, once
    void DispatchSkyViewLUT(RHI::CommandList cl);
    void DispatchAerialPerspective(RHI::CommandList cl);
    void DispatchAtmosphere(RHI::CommandList cl);
    void DispatchPrefilter (RHI::CommandList cl, uint64_t sourceSrv);
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    RHI::GPUBuffer     m_cb;           // UPLOAD — SHConstants
    void*              m_cbMapped = nullptr;

    RHI::GPUBuffer     m_shBuffer;     // UAV + SRV — 9 float4 coefficients
    uint64_t           m_shUavHandle = 0;
    uint64_t           m_shSrvHandle = 0;
    RHI::ResourceState m_shState     = RHI::ResourceState::UNORDERED_ACCESS;

    uint64_t           m_srcCubemap  = 0;
    bool               m_shValid     = false;

    // ---- Specular pre-filter (Phase 2) ---------------------------------------
    RHI::PipelineState m_prefilterPSO;
    RHI::GPUBuffer     m_prefilterCB;       // UPLOAD — N×256 for N mips
    void*              m_prefilterCBMapped = nullptr;

    // Single UAV-capable cubemap (128×128×6, 7 mips, R16G16B16A16_FLOAT).
    // Per-mip array UAVs + TextureCube SRV are auto-created by the RHI when
    // the TEXTURECUBE misc flag + UAV bind flag + mip_levels > 1 are set.
    RHI::Texture       m_specTex;
    RHI::ResourceState m_specState = RHI::ResourceState::UNORDERED_ACCESS;
    uint64_t           m_specSrvHandle = 0;

    uint64_t                               m_lastBakedSrc  = 0; // source handle that was last prefiltered
    bool                                   m_specValid     = false;
    // Temporal prefilter state: after the first full bake (all 6 faces) the
    // dispatch rotates through ONE face per frame, reducing per-frame compute
    // cost to ≈ 1/6 of the "all faces every frame" path.
    uint32_t                               m_prefilterFaceCycle = 0;
    bool                                   m_prefilterNeedsFullBake = true;

    // Dirty-tracking — when the sun hasn't moved and brightness hasn't
    // changed, every per-frame dispatch (SkyView + atmosphere cube + SH +
    // prefilter) reproduces the same output. Skip them entirely.
    DirectX::XMFLOAT3 m_lastBakedSunDir        { 0.0f, 0.0f, 0.0f };
    float             m_lastBakedSunIntensity  = -1.0f;
    uint32_t          m_framesSinceCubeUpdate  = 0xFFFFFFFFu; // large = needs update

    // ---- Atmosphere (Phase 3) ------------------------------------------------
    bool                                   m_atmosphereEnabled = false;
    SkyboxSource                           m_skyboxSource      = SkyboxSource::Atmosphere;
    RHI::PipelineState                     m_atmospherePSO;
    RHI::GPUBuffer                         m_atmosphereCB;
    void*                                  m_atmosphereCBMapped = nullptr;

    RHI::Texture       m_atmosphereTex;
    RHI::ResourceState m_atmosphereState = RHI::ResourceState::UNORDERED_ACCESS;
    uint64_t           m_atmosphereSrvHandle = 0;

    // "Active body" — the direction + colour the rest of the engine treats as
    // the directional light. This is the SUN by day and the MOON by night.
    DirectX::XMFLOAT3                      m_sunDir   { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3                      m_sunColor { 10.0f, 10.0f, 10.0f };
    // "Geometric sun" — the true sun direction even when below horizon, with
    // colour forced to zero below horizon. The atmosphere / aerial-perspective
    // updates use this so the sky stays dark at night instead of computing
    // Rayleigh scattering from the moon (which would produce a faint blue
    // daylight dome — incorrect, since real moonlight is too dim to scatter).
    DirectX::XMFLOAT3                      m_atmosphereSunDir   { 0, 1, 0 };
    DirectX::XMFLOAT3                      m_atmosphereSunColor { 0, 0, 0 };
    bool                                   m_isMoon            = false;
    float                                  m_moonIntensityScale = 0.015f; // ~1.5% of sun
    // Skybox-only moon state — always tracks the moon's position (= -sunDir)
    // regardless of which body drives the lighting, and stays visible for a
    // few degrees below horizon so the disk fades out smoothly instead of
    // popping off at exactly 0°.
    DirectX::XMFLOAT3                      m_moonDir           { 0, -1, 0 };
    DirectX::XMFLOAT3                      m_moonColor         { 0.55f, 0.70f, 1.0f };
    bool                                   m_moonDiskVisible   = false;

    // ---- Hillaire LUTs -------------------------------------------------------
    RHI::PipelineState                     m_transmittancePSO;
    RHI::PipelineState                     m_multiScatterPSO;
    RHI::PipelineState                     m_skyViewPSO;

    RHI::Texture       m_transmittanceTex;
    RHI::ResourceState m_transmittanceState = RHI::ResourceState::UNORDERED_ACCESS;

    RHI::Texture       m_multiScatterTex;
    RHI::ResourceState m_multiScatterState = RHI::ResourceState::UNORDERED_ACCESS;

    RHI::Texture       m_skyViewTex;
    RHI::ResourceState m_skyViewState = RHI::ResourceState::UNORDERED_ACCESS;

    // Shared CB pool for per-dispatch constants (4 × 256B slots).
    RHI::GPUBuffer                         m_atmoLutCB;
    void*                                  m_atmoLutCBMapped = nullptr;

    bool                                   m_staticLutsBaked = false;

    // Time-of-day state.
    bool  m_timeOfDayEnabled = false;
    float m_timeOfDay        = 0.35f;         // just past sunrise by default
    float m_timeSpeed        = 0.0f;          // paused by default
    float m_latitudeRad      = 0.6f;          // ~34° N, mid-latitude
    float m_sunIntensityScale = 1.0f;         // master sun / sky brightness
    // Global IBL multiplier. Default 0.1 — IBL at full strength on a default
    // sky was overwhelming the direct-light contribution; 0.1 gives a subtle
    // ambient fill without flattening the directional shading. Editor slider
    // (or per-scene .ippc) raises if a scene needs full IBL.
    float m_iblStrength       = 0.1f;
    // Flat ambient base — baseline indirect contribution before IBL/probes.
    // Default 0 so unlit faces stay genuinely dark and IBL/probe diffuse is
    // the only indirect source. Editor slider can dial back up if a scene
    // needs a fill term (typically a faint sky-tint, ~0.05-0.1 luma).
    DirectX::XMFLOAT3 m_ambientColor { 0.0f, 0.0f, 0.0f };

    // ---- Aerial Perspective -------------------------------------------------
    RHI::PipelineState m_aerialPSO;
    RHI::Texture       m_aerialTex;
    // Start in SHADER_RESOURCE so the pixel shader can safely sample even
    // before the first dispatch completes (reads the cleared-on-create zeros).
    RHI::ResourceState m_aerialState     = RHI::ResourceState::SHADER_RESOURCE;
    uint64_t           m_aerialSrvHandle = 0;
    bool                                   m_aerialValid     = false;
    // Off by default — the AP composite is easy to get wrong (depends on the
    // engine's world-unit-to-km scale). User enables via the editor when the
    // scene is big enough for distance fog to matter.
    bool                                   m_aerialCompositeEnabled = false;

    DirectX::XMFLOAT3 m_cameraPosWorld     { 0, 0, 0 };
    DirectX::XMFLOAT3 m_cameraForwardWorld { 0, 0, 1 };
    float             m_invViewProj[16]    { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
};
