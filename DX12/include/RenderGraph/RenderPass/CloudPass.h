#pragma once

// CloudPass — MVP volumetric clouds.
// -----------------------------------------------------------------------------
// Pipeline (per frame, when enabled):
//   0. First frame ONCE: CloudNoiseBake compute  → 128³ R8 Worley+Perlin
//   1. CloudRaymarch    compute (quarter-res)    → RGBA16F (rgb=premult scatter,
//                                                            a=transmittance)
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
        float density;           float noiseScale;
        float anisotropy;        float extinction;                // row 6

        float sunDir[3];         float ambientStrength;           // row 7
        float sunColor[3];       float _pad0;                     // row 8
        float cloudColor[3];     float _pad1;                     // row 9
        float windOffset[3];     float _pad2;                     // row 10

        float halfResW;          float halfResH;
        float fullResW;          float fullResH;                  // row 11
    };

private:
    static constexpr uint32_t kNoiseDim    = 128;
    static constexpr uint32_t kRaymarchSteps = 64;

    bool m_enabled = false;
    bool m_viewModeHidden = false;   // Renderer hides clouds in Wireframe view

    RG::RGTextureHandle m_depth;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Compute PSOs
    RHI::PipelineState m_noiseBakePSO;
    RHI::PipelineState m_raymarchPSO;

    // Noise volume — 128^3 R8 single channel, baked once on first Execute.
    RHI::Texture       m_noiseTex;
    RHI::ResourceState m_noiseState = RHI::ResourceState::UNORDERED_ACCESS;
    bool               m_noiseBaked = false;

    // Quarter-res raymarch output — RGBA16F.
    RHI::Texture       m_cloudTex;
    RHI::ResourceState m_cloudState = RHI::ResourceState::UNORDERED_ACCESS;
    uint32_t           m_cloudW = 0, m_cloudH = 0;
    uint32_t           m_lastFullW = 0, m_lastFullH = 0;

    // Constant buffers
    FrameCB<CloudConstants> m_cb;

    // Linear sampler for noise + composite upsample.
    int            m_linearSamplerSlot = -1;

    // Per-frame state captured by setters.
    DirectX::XMFLOAT4X4 m_invViewProj{};
    DirectX::XMFLOAT3   m_cameraPos{};
    float               m_nearZ = 0.f;
    float               m_farZ  = 0.f;
    DirectX::XMFLOAT3   m_sunDir   { 0.f, 1.f, 0.f };
    DirectX::XMFLOAT3   m_sunColor { 1.f, 1.f, 1.f };

    // Cloud author state.
    bool                m_paramsEnabled = false;
    float               m_bottomAlt     = 1500.f;
    float               m_topAlt        = 4000.f;
    float               m_coverage      = 0.55f;
    float               m_density       = 1.f;
    float               m_noiseScale    = 0.0008f;
    DirectX::XMFLOAT3   m_windDir       { 1.f, 0.f, 0.f };
    float               m_windSpeed     = 8.f;
    float               m_windOffset    = 0.f;  // accumulated m
    float               m_anisotropy    = 0.55f;
    float               m_extinction    = 0.08f;
    float               m_ambient       = 0.35f;
    DirectX::XMFLOAT3   m_cloudColor    { 1.f, 1.f, 1.f };

    void RebuildCloudTexture(uint32_t fullW, uint32_t fullH);
    void BakeNoiseOnce(RHI::CommandList cl);
    void UploadCB();
    void DispatchRaymarch(RHI::CommandList cl);
    void Composite(RHI::CommandList cl);
};
