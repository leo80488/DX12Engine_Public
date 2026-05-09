#pragma once

// VolumetricFogPass — Froxel-based volumetric fog (sun + height fog).
// -----------------------------------------------------------------------------
// Pipeline (per frame, when enabled):
//   1. FroxelDensity     compute  → 3D R16F4 (.rgb scattering, .a extinction)
//   2. FroxelLightInject compute  → 3D R16F4 (.rgb σ_s × L_in,  .a extinction)
//   3. FroxelScatter     compute  → 3D R16F4 (.rgb ∫T·L,        .a transmittance)
//   4. VolumetricApply   graphics → blends scatter + scene HDR in a fullscreen
//                                   triangle (drawn AFTER the lighting pass,
//                                   BEFORE bloom/tone-map).
//
// All settings live on the pass — Editor can toggle / tune them (see
// EditorLayer Post-Processing panel). v1 ignores per-light components and
// just injects the engine's primary directional sun (CSM-shadowed).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"

#include <vector>
#include <DirectXMath.h>

class VolumetricFogPass : public RG::RenderPass
{
public:
    VolumetricFogPass(RG::RGTextureHandle depth);
    ~VolumetricFogPass();

    const char* GetName() const override { return "VolumetricFogPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // ---- Editor / Renderer-side configuration ------------------------------
    void  SetEnabled(bool on)      { m_enabled = on; }
    bool  IsEnabled() const        { return m_enabled; }

    void  SetDensity(float d)      { m_fogDensity = d; }
    float GetDensity() const       { return m_fogDensity; }

    void  SetScattering(float s)   { m_fogScattering = s; }
    float GetScattering() const    { return m_fogScattering; }

    void  SetAbsorption(float a)   { m_fogAbsorption = a; }
    float GetAbsorption() const    { return m_fogAbsorption; }

    void  SetAnisotropy(float g)   { m_anisotropy = g; }
    float GetAnisotropy() const    { return m_anisotropy; }

    void  SetHeight(float start, float falloff) { m_heightStart = start; m_heightFalloff = falloff; }
    float GetHeightStart() const   { return m_heightStart; }
    float GetHeightFalloff() const { return m_heightFalloff; }

    void  SetRange(float nearKm, float farKm) { m_froxelNear = nearKm; m_froxelFar = farKm; }
    float GetRangeNear() const     { return m_froxelNear; }
    float GetRangeFar()  const     { return m_froxelFar;  }

    void  SetAmbient(const DirectX::XMFLOAT3& col, float contrib)
    { m_ambientColor = col; m_ambientContribution = contrib; }
    float GetAmbientContribution() const { return m_ambientContribution; }

    void  SetTemporalEnabled(bool on) { m_temporalEnabled = on; }
    bool  IsTemporalEnabled() const   { return m_temporalEnabled; }
    void  SetTemporalAlpha(float a)   { m_temporalAlpha = a; }
    float GetTemporalAlpha() const    { return m_temporalAlpha; }

    // Per-pixel raymarch tier (sun + shadow-casting spots). Complements the
    // froxel grid path (no-shadow lights). Default-on; can be toggled for
    // debugging from the editor.
    void  SetRaymarchEnabled(bool on) { m_rmEnabled = on; }
    bool  IsRaymarchEnabled() const   { return m_rmEnabled; }

  
    /** Per-frame state pushed by Renderer:
     *  - sunDir/sunColor uses LightCB convention (sunDir = FROM ground).
     *  - depth handle is the engine's HDR depth buffer (used by Apply pass).
     *  - shadowSrv is the 3-cascade shadow Texture2DArray GPU handle.
     *  - shadowMatrices/cascadeSplits etc. come from LightCB. */
    void SetFrameState(const DirectX::XMFLOAT4X4& viewProj,
                       const DirectX::XMFLOAT4X4& invViewProj,
                       const DirectX::XMFLOAT4X4& prevViewProj,
                       const DirectX::XMFLOAT3&   cameraPos,
                       float nearPlane, float farPlane,
                       uint32_t frameIndex);

    void SetSun(const DirectX::XMFLOAT3& dir,
                const DirectX::XMFLOAT3& color,
                float strength)
    { m_sunDir = dir; m_sunColor = color; m_sunStrength = strength; }

    // ---- Debug / diagnostic accessors (read by EditorLayer) ---------------
    const DirectX::XMFLOAT3& GetSunDirDebug()   const { return m_sunDir; }
    const DirectX::XMFLOAT3& GetSunColorDebug() const { return m_sunColor; }
    float GetSunStrengthDebug()                 const { return m_sunStrength; }
    uint32_t GetVolumetricLightCount()          const { return m_spotLightCount; }

    void SetShadowState(uint64_t shadowSrvHandle,
                        const DirectX::XMFLOAT4X4 shadowMatrix[3],
                        const DirectX::XMFLOAT3& cascadeSplits,
                        float texelSize, float bias, float strength);

    /** Volumetric-only lights uploaded each frame.
     *  Renderer iterates entities with VolumetricLightComponent (and a
     *  matching LightData) and pushes the resolved per-frame data here.
     *  Without this component a light only contributes to direct surface
     *  lighting — it is invisible inside the fog volume. */
    struct VolLight
    {
        DirectX::XMFLOAT3 position;  float radius;
        DirectX::XMFLOAT3 color;     float intensity;
        DirectX::XMFLOAT3 direction; float spotAngle;
        uint32_t          type;
        uint32_t          shadowSliceIdx; // 0xFFFFFFFF = no shadow map
        uint32_t          _pad[2];
    };
    static_assert(sizeof(VolLight) == 64, "VolLight must match HLSL GPULight layout");
    void SetVolumetricLights(const std::vector<VolLight>& lights);

    /** World-space camera forward (unit). Used by the light-injection shader
     *  to choose the right CSM cascade based on view-Z. */
    void SetCameraForward(const DirectX::XMFLOAT3& fwd) { m_cameraForward = fwd; }

    void SetSceneTargets(uint64_t depthSrvHandle,
                         RG::RGTextureHandle hdrColor)
    { m_depthSrv = depthSrvHandle; m_hdrColor = hdrColor; }

    /** Wire the per-spot-light shadow atlas + per-slice VP matrix buffer
     *  (produced by SpotShadowPass / Renderer). When either handle is 0 the
     *  shader falls back to ScreenSpaceShadow + VoxelOcclusion for every
     *  clustered light, just as before this feature existed. */
    void SetSpotShadowAtlas(uint64_t atlasHandle, uint64_t vpsHandle)
    {
        m_spotShadowAtlasSrv = atlasHandle;
        m_spotShadowVPSrv    = vpsHandle;
    }

    /** Wire the scene-voxel occupancy grid produced by SceneVoxelPass. The
     *  grid bounds feed FroxelConstants so the light-inject shader can raymarch
     *  through the occupancy texture for spot/point light occlusion. Passing
     *  srv == 0 disables the voxel occlusion path (shader takes the fall-back
     *  "always visible" branch, same as before this feature existed). */
    void SetVoxelOcclusion(uint64_t occupancySrvGpuHandle,
                           const DirectX::XMFLOAT3& gridMin,
                           const DirectX::XMFLOAT3& gridMax,
                           uint32_t gridDim)
    {
        m_voxelOccupancySrv = occupancySrvGpuHandle;
        m_voxelGridMin      = gridMin;
        m_voxelGridMax      = gridMax;
        m_voxelGridDim      = gridDim;
    }

    /** Returns the CB so Renderer can register it with the RenderGraph
     *  ("VolApplyCB" → bound to b3 space0 in the apply PS). */
    const RHI::GPUBuffer& GetApplyCB() const { return m_applyCB; }

private:
    static constexpr uint32_t kFroxelW = 160;
    static constexpr uint32_t kFroxelH = 90;
    // Z slice count was 64 — the horizontal banding visible on spot-cone walls
    // is each slice's discrete value. 128 halves the cell size and the
    // visible step → acceptable cost (dispatch ~2× work on scatter + temporal).
    static constexpr uint32_t kFroxelD = 128;

    void Create3DTextures(IGraphicsDevice& gfx);
    void DispatchDensity(RHI::CommandList cl);
    void DispatchLightInject(RHI::CommandList cl);
    void DispatchScatter(RHI::CommandList cl);
    void DispatchTemporal(RHI::CommandList cl);
    void DrawApply(RHI::CommandList cl);

    // Per-pixel raymarch tier.
    void DestroyRaymarchTextures();
    void RebuildRaymarchTextures(uint32_t fullW, uint32_t fullH);
    void DispatchRaymarch(RHI::CommandList cl);
    void DispatchRaymarchTemporal(RHI::CommandList cl);
    void DrawRaymarchApply(RHI::CommandList cl);

    IGraphicsDevice*        m_gfx = nullptr;
    ShaderLibrary           m_shaderLib;
    PSOCache                m_psoCache;

    // Compute PSOs.
    RHI::PipelineState      m_densityPSO;
    RHI::PipelineState      m_lightInjectPSO;
    RHI::PipelineState      m_scatterPSO;
    RHI::PipelineState      m_temporalPSO;

    // 3D textures (RHI). SRV / UAV descriptors live in the backend pool and
    // are queried per-use via IGraphicsDevice::GetTextureSRV/UAVGpuHandle.
    RHI::Texture        m_densityTex;
    RHI::Texture        m_lightingTex;
    RHI::Texture        m_scatteringTex;
    RHI::ResourceState  m_densityState    = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    RHI::ResourceState  m_lightingState   = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    RHI::ResourceState  m_scatteringState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // ---- Temporal reprojection (ping-pong history) -------------------------
    RHI::Texture        m_historyTex[2];
    RHI::ResourceState  m_historyState[2] = {
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE };
    uint32_t                               m_historyReadIdx  = 0;
    uint32_t                               m_historyWriteIdx = 1;
    bool                                   m_historyValid    = false;
    bool                                   m_temporalEnabled = true;
    // 0.05 was too aggressive on history retention — under any camera motion
    // the history became too stale and left smears. 0.12 still weights history
    // heavily (~8-frame decay) but reacts to real scene changes in ~3 frames.
    float                                  m_temporalAlpha   = 0.12f;

    // Two CBs:
    //   m_paramCB  — FroxelParams (matches HLSL FroxelCB layout)
    //   m_shadowCB — CSM matrices + cascade splits + sampling params
    RHI::GPUBuffer  m_paramCB;
    void*           m_paramCBMapped  = nullptr;
    RHI::GPUBuffer  m_shadowCB;
    void*           m_shadowCBMapped = nullptr;
    // Apply-pass CB (graphics root sig at b3 space0). Tiny — only enough state
    // for the PS to recover view-Z from depth and slice the froxel grid.
    RHI::GPUBuffer  m_applyCB;
    void*           m_applyCBMapped  = nullptr;
    int             m_shadowSampler  = -1;       // CMP < sampler for CSM
    int             m_linearSampler  = -1;       // for trilinear scattering / depth read

    // Scene-side state (per-frame).
    RG::RGTextureHandle m_depth;                  // declared in Setup() so graph emits SRV barrier
    RG::RGTextureHandle m_hdrColor{};
    uint64_t            m_depthSrv = 0;
    bool                m_enabled  = false;

    // Sun / shadow.
    DirectX::XMFLOAT3      m_sunDir       { 0, -1, 0 };
    DirectX::XMFLOAT3      m_sunColor     { 1, 1, 1 };
    float                  m_sunStrength  = 5.0f;
    uint64_t               m_shadowSrv    = 0;
    DirectX::XMFLOAT4X4    m_shadowMatrix[3]{};
    DirectX::XMFLOAT3      m_cascadeSplits{ 10, 50, 200 };
    float                  m_shadowTexelSize = 1.0f / 2048.0f;
    float                  m_shadowBias      = 0.0005f;
    float                  m_shadowStrength  = 1.0f;
    DirectX::XMFLOAT3      m_cameraForward   { 0, 0, 1 };

    // Spot-light shadow atlas SRVs (t6 / t7 space2). Set by Renderer each frame.
    // Default 0 → shader falls back to ScreenSpaceShadow + VoxelOcclusion.
    uint64_t                m_spotShadowAtlasSrv = 0;
    uint64_t                m_spotShadowVPSrv    = 0;

    // Voxel occupancy grid (produced by SceneVoxelPass, consumed by LightInject).
    // Default 0 SRV / zero extent → shader falls back to "visible" everywhere.
    uint64_t                m_voxelOccupancySrv = 0;
    DirectX::XMFLOAT3       m_voxelGridMin      { 0, 0, 0 };
    DirectX::XMFLOAT3       m_voxelGridMax      { 0, 0, 0 };
    uint32_t                m_voxelGridDim      = 0;

    // Volumetric-only lights — separate UPLOAD buffer (max kMaxVolLights).
    static constexpr uint32_t kMaxVolLights = 16;
    RHI::GPUBuffer         m_volLightsBuf;
    void*                  m_volLightsBufMapped = nullptr;
    uint64_t               m_lightsSrv          = 0;  // SRV of m_volLightsBuf
    uint32_t               m_spotLightCount     = 0;

    // Per-frame: which 3D SRV the apply PS samples (scattering vs history[w]).
    uint64_t               m_applyOverrideSrv = 0;

    // Per-frame CB values (filled by Renderer via SetFrameState).
    DirectX::XMFLOAT4X4    m_viewProj{};
    DirectX::XMFLOAT4X4    m_invViewProj{};
    DirectX::XMFLOAT4X4    m_prevViewProj{};
    DirectX::XMFLOAT3      m_cameraPos { 0, 0, 0 };
    float                  m_nearPlane = 0.1f;
    float                  m_farPlane  = 1000.0f;
    uint32_t               m_frameIndex = 0;

    // On the very first SetFrameState call, m_prevViewProj is still identity
    // → temporal reprojection UVs are garbage → history always misses and
    // every frame effectively runs with alpha=1. Track the first call so we
    // can seed prevViewProj with the current matrix instead.
    bool                   m_firstFrameDone = false;
    // Previous frame's camera position — used to detect large teleports
    // (scene switch, debug cam jump) and bypass temporal blending for that
    // frame. Kept here rather than on the component side so the check lives
    // next to the data it guards.
    DirectX::XMFLOAT3      m_prevCameraPos { 0, 0, 0 };

    // ---- Per-pixel raymarch tier -------------------------------------------
    // Half-resolution 2D textures; rebuilt on viewport size change.
    RHI::PipelineState  m_rmPSO;
    RHI::PipelineState  m_rmTemporalPSO;
    RHI::Texture        m_rmCurrentTex;
    RHI::ResourceState  m_rmCurrentState = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::Texture        m_rmHistoryTex[2];
    RHI::ResourceState  m_rmHistoryState[2] = {
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::UNORDERED_ACCESS };
    uint32_t            m_rmHistoryReadIdx  = 0;
    uint32_t            m_rmHistoryWriteIdx = 1;
    bool                m_rmHistoryValid    = false;
    uint32_t            m_rmHalfW           = 0;
    uint32_t            m_rmHalfH           = 0;
    uint32_t            m_rmLastFullW       = 0;
    uint32_t            m_rmLastFullH       = 0;
    bool                m_rmEnabled         = true;

    // Editable fog params.
    float                  m_fogDensity         = 0.02f;
    float                  m_fogScattering      = 0.6f;
    float                  m_fogAbsorption      = 0.02f;
    float                  m_anisotropy         = 0.4f;
    float                  m_heightStart        = 0.0f;
    float                  m_heightFalloff      = 0.05f;
    float                  m_froxelNear         = 0.5f;
    float                  m_froxelFar          = 80.0f;
    DirectX::XMFLOAT3      m_ambientColor       { 0.4f, 0.55f, 0.85f };
    float                  m_ambientContribution = 0.4f;
};
