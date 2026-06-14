#pragma once

// CloudPass — Nubis/Frostbite-style volumetric clouds.
// -----------------------------------------------------------------------------
// Pipeline (per frame, when enabled):
//   0. First frame ONCE, three bakes:
//        CloudNoiseBake        → 128³ RGBA8 base shape (R=Perlin-Worley,
//                                GBA=Worley FBM octaves)
//        CloudDetailNoiseBake  → 32³  RGBA8 high-freq Worley erosion
//        CloudWeatherBake      → 512² RGBA8 weather map (coverage/fill/type)
//   1. CloudRaymarch    compute (quarter-res)    → RGBA16F (rgb=premult scatter,
//                                                            a=transmittance)
//      Spherical-shell march, adaptive coarse/fine stepping, IGN jitter,
//      multi-scatter octaves + dual-lobe HG + energy-conserving integration.
//   2. CloudComposite   graphics (fullscreen tri) → blends quarter-res result
//                                                   into HDR (src=ONE,
//                                                   dst=SRC_ALPHA — alpha-over
//                                                   with cloud transmittance).
//
// Sun direction + colour are pushed by Renderer each frame from
// TODOutputComponent (or from the user's directional light when TOD is off).
// All authoring lives on CloudComponent — Renderer reads it and calls
// SetParams().

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <vector>
#include <DirectXMath.h>

struct CloudComponent;

class CloudPass : public RG::RenderPass
{
public:
    explicit CloudPass(RG::RGTextureHandle depth);
    ~CloudPass();

    const char* GetName() const override { return "CloudPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // ---- Renderer-pushed per-frame state ----------------------------------
    void SetEnabled(bool on) { m_enabled = on; }
    bool IsEnabled() const   { return m_enabled; }

    // Global view-mode suppress — hide clouds in Wireframe view for a clean
    // dark background. Independent of SetEnabled so it never clobbers the
    // CloudComponent-driven enable state. Set every frame by Renderer.
    void SetViewModeHidden(bool h) { m_viewModeHidden = h; }

    /** Per-frame camera. invViewProj uses LightCB convention (row-vector
     *  matrix, applied via mul(ndc4, invViewProj) → world.w-divided). */
    void SetCamera(const DirectX::XMFLOAT4X4& invViewProj,
                   const DirectX::XMFLOAT3&   cameraPos,
                   float nearZ, float farZ);

    /** Sun direction TOWARDS the sun (unit). Colour is linear-RGB radiance —
     *  same convention as TODOutput.activeColor / sunColor. */
    void SetSun(const DirectX::XMFLOAT3& dir, const DirectX::XMFLOAT3& color);

    /** Push every CloudComponent author field. dt advances the wind offset. */
    void SetParams(const CloudComponent& c, float dt);

    /** Current-frame slot of the triple-buffered CloudConstants CB; Renderer
     *  registers it with the RenderGraph each frame under "CloudApplyCB"
     *  (bound to b3 space0 in the composite PS). Rebind every frame — the
     *  underlying buffer rotates. */
    const RHI::GPUBuffer& GetCloudCB(IGraphicsDevice& gfx) const { return m_cb.CurrentBuffer(gfx); }

    /** SRV of the quarter-res raymarch output (a = view-ray transmittance)
     *  — consumed by LensFlarePass for sun occlusion. Returns 0 unless the
     *  raymarch actually ran THIS frame (disabled / wireframe / failed-PSO
     *  frames must not serve stale overcast). */
    uint64_t GetCloudSrvHandle() const;

public:
    // HLSL CB layout — keep aligned with CloudRaymarch.cs.hlsl.
    // Each row is EXACTLY 16 bytes. HLSL silently pads before a float3 if
    // the previous row leaves <12 bytes — so we explicitly pack 4 scalars
    // per row whenever a float3 follows.
    struct alignas(16) CloudConstants
    {
        float invViewProj[16];                                    // 64 (row 0..3)

        float cameraPos[3];      float nearZ;                     // row 4
        float farZ;              float bottomAltitude;
        float topAltitude;       float coverage;                  // row 5
        float density;           float baseNoiseScale;
        float detailNoiseScale;  float detailStrength;            // row 6
        float weatherScale;      float cloudTypeBias;
        float anvilBias;         float extinction;                // row 7

        float sunDir[3];         float ambientStrength;           // row 8
        float sunColor[3];       float phaseFwdG;                 // row 9
        float ambientTint[3];    float phaseBackG;                // row 10
        float cloudColor[3];     float phaseBlend;                // row 11
        float windOffset[3];     float silverIntensity;           // row 12

        float silverSpread;      float frameIndex;
        float maxSteps;          float maxTraceDist;              // row 13

        float halfResW;          float halfResH;
        float fullResW;          float fullResH;                  // row 14
    };

private:
    static constexpr uint32_t kBaseNoiseDim   = 128;
    static constexpr uint32_t kDetailNoiseDim = 32;
    static constexpr uint32_t kWeatherDim     = 512;
    // Max march distance from the shell entry point (UE default mode).
    static constexpr float    kMaxTraceDist   = 30000.0f;

    bool m_enabled = false;
    bool m_viewModeHidden = false;   // Renderer hides clouds in Wireframe view

    RG::RGTextureHandle m_depth;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Compute PSOs
    RHI::PipelineState m_noiseBakePSO;
    RHI::PipelineState m_detailBakePSO;
    RHI::PipelineState m_weatherBakePSO;
    RHI::PipelineState m_raymarchPSO;

    // Baked-once lookup textures.
    RHI::Texture       m_baseNoiseTex;    // 128³ RGBA8
    RHI::Texture       m_detailNoiseTex;  // 32³  RGBA8
    RHI::Texture       m_weatherTex;      // 512² RGBA8
    RHI::ResourceState m_baseNoiseState   = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_detailNoiseState = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_weatherState     = RHI::ResourceState::UNORDERED_ACCESS;
    bool               m_noiseBaked = false;

    // Quarter-res raymarch output — RGBA16F.
    RHI::Texture       m_cloudTex;
    RHI::ResourceState m_cloudState = RHI::ResourceState::UNORDERED_ACCESS;
    // Quarter-res scene distance each texel marched against — R32F, feeds
    // the composite's depth-aware upsample weights.
    RHI::Texture       m_cloudDistTex;
    RHI::ResourceState m_cloudDistState = RHI::ResourceState::UNORDERED_ACCESS;
    uint32_t           m_cloudW = 0, m_cloudH = 0;
    uint32_t           m_lastFullW = 0, m_lastFullH = 0;
    // True only between a successful DispatchRaymarch and the next Execute —
    // gates GetCloudSrvHandle so consumers never read stale frames.
    bool               m_renderedThisFrame = false;

    // Constant buffers
    FrameCB<CloudConstants> m_cb;

    // Linear sampler for the composite upsample.
    int            m_linearSamplerSlot = -1;

    // Monotonic frame counter (mod 8) driving the IGN jitter sequence — TAA
    // runs after the cloud composite and accumulates it.
    uint32_t       m_frameIndex = 0;

    // Per-frame state captured by setters.
    DirectX::XMFLOAT4X4 m_invViewProj{};
    DirectX::XMFLOAT3   m_cameraPos{};
    float               m_nearZ = 0.f;
    float               m_farZ  = 0.f;
    DirectX::XMFLOAT3   m_sunDir   { 0.f, 1.f, 0.f };
    DirectX::XMFLOAT3   m_sunColor { 1.f, 1.f, 1.f };

    // Cloud author state (defaults mirror CloudComponent).
    bool                m_paramsEnabled = false;
    float               m_bottomAlt     = 1500.f;
    float               m_topAlt        = 4000.f;
    float               m_coverage      = 0.5f;
    float               m_density       = 1.f;
    float               m_baseNoiseScale   = 0.00025f;
    float               m_detailNoiseScale = 0.002f;
    float               m_detailStrength   = 0.30f;
    float               m_weatherScale     = 0.00002f;
    float               m_cloudTypeBias    = 0.f;
    float               m_anvilBias        = 0.f;
    DirectX::XMFLOAT3   m_windDir       { 1.f, 0.f, 0.f };
    float               m_windSpeed     = 8.f;
    float               m_windOffset    = 0.f;  // accumulated m
    float               m_phaseFwdG     = 0.6f;
    float               m_phaseBackG    = -0.2f;
    float               m_phaseBlend    = 0.3f;
    float               m_silverIntensity = 0.8f;
    float               m_silverSpread    = 0.25f;
    float               m_extinction    = 0.05f;
    float               m_ambient       = 0.5f;
    DirectX::XMFLOAT3   m_ambientTint   { 0.55f, 0.65f, 0.85f };
    DirectX::XMFLOAT3   m_cloudColor    { 1.f, 1.f, 1.f };
    float               m_maxSteps      = 96.f;

    void RebuildCloudTexture(uint32_t fullW, uint32_t fullH);
    void BakeNoiseOnce(RHI::CommandList cl);
    void UploadCB();
    void DispatchRaymarch(RHI::CommandList cl);
    void Composite(RHI::CommandList cl);
};
