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
#include "Graphics/FrameCB.h"

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

    /** Active light direction / colour — the body driving direct lighting
     *  (sun by day, moon by night). LightCB convention: `dir` points
     *  TOWARDS the body. Pushed each frame by Renderer from the SunLightTag
     *  entity (or via TODOutput when TOD is active). */
    void SetSunDir(const DirectX::XMFLOAT3& dir, const DirectX::XMFLOAT3& color)
    {
        m_sunDir = dir;
        m_sunColor = color;
    }
    const DirectX::XMFLOAT3& GetSunDir()   const { return m_sunDir; }
    const DirectX::XMFLOAT3& GetSunColor() const { return m_sunColor; }

    /** Geometric sun direction / colour — true sun position even when below
     *  horizon, with colour zero below horizon. Consumed by the atmosphere /
     *  aerial-perspective shaders so Rayleigh scattering goes dark at night
     *  instead of computing a faint blue dome from the moon's direction. */
    void SetAtmosphereSun(const DirectX::XMFLOAT3& dir, const DirectX::XMFLOAT3& color)
    {
        m_atmosphereSunDir   = dir;
        m_atmosphereSunColor = color;
    }

    /** Global IBL strength. Master scale on indirect lighting (both diffuse
     *  and specular IBL contributions in Lighting.ps). 0 = no IBL at all. */
    void SetIBLStrength(float s)   { m_iblStrength = s; }
    float GetIBLStrength() const   { return m_iblStrength; }

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

    // CB pool strides / slot counts — kept here so the multi-slot pool structs
    // below can size themselves at compile time. Definitions live in
    // SkyIBLPass.cpp; redeclared here as static constexpr for sizing only.
    static constexpr uint32_t kPrefilterCBStride = 256;
    static constexpr uint32_t kPrefilterCBSlots  = kSpecularMips * 6;
    static constexpr uint32_t kLutCBStride       = 512;
    static constexpr uint32_t kLutCBSlots        = 5;

    // Multi-slot CB pool wrappers — see BloomPass.h for the established pattern.
    // Each pool is one buffer holding N × stride bytes; per-dispatch writes
    // target a unique slot so CPU writes never overwrite data the GPU is still
    // reading from a previous in-flight frame's binding.
    struct alignas(256) PrefilterCBPool { uint8_t bytes[kPrefilterCBStride * kPrefilterCBSlots]; };
    struct alignas(256) AtmoLutCBPool   { uint8_t bytes[kLutCBStride       * kLutCBSlots];      };

    struct alignas(16) SHConstants
    {
        uint32_t sampleCount;
        uint32_t _pad[3];
    };
    FrameCB<SHConstants> m_cb;          // UPLOAD — SHConstants

    struct alignas(16) AtmosphereConstants
    {
        float    sunDir[3];    float    _pad0;
        float    sunColor[3];  uint32_t faceSize;
        float    cameraAltitudeKm;
        float    _pad1;
        float    _pad2;
        float    _pad3;
    };

    RHI::GPUBuffer     m_shBuffer;     // UAV + SRV — 9 float4 coefficients
    uint64_t           m_shUavHandle = 0;
    uint64_t           m_shSrvHandle = 0;
    RHI::ResourceState m_shState     = RHI::ResourceState::UNORDERED_ACCESS;

    uint64_t           m_srcCubemap  = 0;
    bool               m_shValid     = false;

    // ---- Specular pre-filter (Phase 2) ---------------------------------------
    RHI::PipelineState m_prefilterPSO;
    // UPLOAD — kSpecularMips × 6 × 256 bytes, one slot per (face, mip) pair.
    FrameCB<PrefilterCBPool> m_prefilterCB;

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
    FrameCB<AtmosphereConstants>           m_atmosphereCB;       // UPLOAD — AtmosphereConstants

    RHI::Texture       m_atmosphereTex;
    RHI::ResourceState m_atmosphereState = RHI::ResourceState::UNORDERED_ACCESS;
    uint64_t           m_atmosphereSrvHandle = 0;

    // "Active body" — the direction + colour the rest of the engine treats as
    // the directional light (SUN by day, MOON by night). Pushed by Renderer
    // from the SunLightTag entity (or TODOutput when TOD is active).
    DirectX::XMFLOAT3                      m_sunDir   { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3                      m_sunColor { 10.0f, 10.0f, 10.0f };
    // "Geometric sun" — true sun direction even when below horizon, with
    // colour forced to zero below horizon. Atmosphere / aerial-perspective
    // sample this so the sky stays dark at night. Pushed via SetAtmosphereSun.
    DirectX::XMFLOAT3                      m_atmosphereSunDir   { 0, 1, 0 };
    DirectX::XMFLOAT3                      m_atmosphereSunColor { 0, 0, 0 };

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

    // Shared CB pool for per-dispatch atmosphere LUT constants
    // (5 × 512B slots: Transmittance / MultiScatter / SkyView / [unused Atmo] / Aerial).
    FrameCB<AtmoLutCBPool> m_atmoLutCB;

    bool                                   m_staticLutsBaked = false;

    // Global IBL multiplier. Default 0.1 — IBL at full strength on a default
    // sky was overwhelming the direct-light contribution; 0.1 gives a subtle
    // ambient fill without flattening the directional shading. Editor slider
    // (or per-scene .ippc) raises if a scene needs full IBL.
    float m_iblStrength       = 0.1f;

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
