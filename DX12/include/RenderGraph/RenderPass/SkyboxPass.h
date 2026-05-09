#pragma once

// SkyboxPass — renders the environment cubemap as a skybox after the lighting pass.
//
// Uses the w=0 vertex trick: mul(float4(pos, 0.0f), viewProj).xyww sets NDC z = w = 1
// so the skybox sits at the far plane.  Combined with LESS_EQUAL depth test the sky only
// draws where no scene geometry was rendered.  Cull NONE renders the inside of the cube.
//
// The environment cubemap is supplied each frame via SetEnvMap() called by Renderer.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"

#include <cmath>
#include <DirectXMath.h>

namespace RG { class RenderContext; }

class SkyboxPass : public RG::RenderPass
{
public:
    explicit SkyboxPass(RG::RGTextureHandle depth);

    const char* GetName() const override { return "SkyboxPass"; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot-reload — base-class default impl walks these accessors.
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    /** Called by Renderer each frame with the prefiltered radiance cubemap GPU handle.
     *  Pass 0 to skip the skybox draw. */
    void SetEnvMap(uint64_t gpuHandle) { m_envMapGpuHandle = gpuHandle; }

    /** Borrow the 36-vertex cube ByteAddressBuffer so external passes
     *  (e.g. reflection probe capture) can reuse the same geometry without
     *  re-uploading. Returns an invalid buffer before Init() has run. */
    const RHI::GPUBuffer& GetCubeVB() const { return m_cubeVB; }

    /** Per-frame sun parameters for the analytic sun disk.
     *  `dir` unit vector towards the sun; `color` linear radiance (already
     *  includes time-of-day attenuation); `diskHalfAngleRad` controls the
     *  angular radius of the visible disk (~0.0087 rad = 0.5°, widened
     *  slightly for a cleaner silhouette). */
    void SetSun(const DirectX::XMFLOAT3& dir,
                const DirectX::XMFLOAT3& color,
                float diskHalfAngleRad,
                float diskIntensity)
    {
        m_sunDir           = dir;
        m_sunColor         = color;
        m_sunDiskCos       = std::cos(diskHalfAngleRad);
        m_sunDiskIntensity = diskIntensity;
    }

    /** Per-frame moon state. The moon disk is drawn whenever `visible` is true
     *  and `texHandle` is non-zero — its direction and colour are tracked
     *  independently of m_sunDir so the shader can continue drawing the moon
     *  as it descends below horizon (and before it has formally taken over
     *  as the directional light). `dir` points TOWARDS the moon, unit. */
    void SetMoon(uint64_t texHandle,
                 bool visible,
                 const DirectX::XMFLOAT3& dir,
                 const DirectX::XMFLOAT3& color,
                 float diskHalfAngleRad)
    {
        m_moonTexHandle = texHandle;
        m_moonVisible   = visible;
        m_moonDir       = dir;
        m_moonColor     = color;
        m_moonDiskCos   = std::cos(diskHalfAngleRad);
    }

    /** Per-frame starfield state. `intensity` is the day/night blend
     *  ([0..1], 0 = invisible / day, 1 = full night). `time` accumulates
     *  game seconds and drives per-star twinkle. The other two are tuning
     *  knobs (Editor can expose later if desired).
     *
     *  Pass intensity == 0 to skip the star rendering branch entirely. */
    void SetStars(float intensity, float time,
                  float density = 256.0f, float brightness = 0.7f)
    {
        m_starIntensity  = intensity;
        m_starTime       = time;
        m_starDensity    = density;
        m_starBrightness = brightness;
    }

    /** Returns the CB so Renderer can register it with the RenderGraph. */
    const RHI::GPUBuffer& GetSkyCB() const { return m_skyCB; }

private:
    PSODesc BuildPSODesc() const;

    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;
    int                 m_sampler = -1;
    RG::RGTextureHandle m_depth;

    // Cube vertex buffer — 36 float3 positions (pre-expanded, no index buffer).
    RHI::GPUBuffer      m_cubeVB;

    // IBL environment cubemap — set by Renderer via SetEnvMap() each frame.
    uint64_t            m_envMapGpuHandle = 0;

    // Sun disk state (written each frame via SetSun, uploaded into m_skyCB).
    RHI::GPUBuffer      m_skyCB;
    void*               m_skyCBMapped = nullptr;
    DirectX::XMFLOAT3   m_sunDir           { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3   m_sunColor         { 0.0f, 0.0f, 0.0f };
    float               m_sunDiskCos       = 0.99998f;   // cos(0.5°)
    float               m_sunDiskIntensity = 40.0f;

    // Moon path — disk is drawn whenever the moon is within the horizon-fade
    // band, not gated on the "active body" lighting switch.
    uint64_t            m_moonTexHandle    = 0;
    bool                m_moonVisible      = false;
    DirectX::XMFLOAT3   m_moonDir          { 0, -1, 0 };
    DirectX::XMFLOAT3   m_moonColor        { 0.55f, 0.70f, 1.0f };
    float               m_moonDiskCos      = 0.9994f;    // cos(~2°), wider than sun

    // Starfield state — Renderer pushes per frame via SetStars().
    float               m_starIntensity    = 0.0f;
    float               m_starTime         = 0.0f;
    float               m_starDensity      = 256.0f;
    float               m_starBrightness   = 0.5f;

    // Pre-baked star cubemap. Filled by a one-time compute dispatch on the
    // first Execute() call, then sampled every frame by the skybox PS at t18
    // space0 (rebound from SSAO's lighting slot — skybox doesn't read SSAO,
    // and the lighting pass rebinds it next frame).
    static constexpr uint32_t kStarsCubeSize = 1024;          // texels per face
    RHI::Texture        m_starsCubemap;
    RHI::ResourceState  m_starsCubemapState = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::PipelineState  m_starsBakePSO;
    RHI::GPUBuffer      m_starsBakeCB;
    void*               m_starsBakeCBMapped = nullptr;
    bool                m_starsBaked        = false;
    int                 m_starsCubeSampler  = -1;        // wrap-clamp for cubemap
};
