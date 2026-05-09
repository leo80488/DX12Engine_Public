#pragma once

// LightingPass — fullscreen deferred PBR lighting (3 stencil-gated PSO variants:
// PBR=1, NPR=2, Unlit=3). Reads GBuffer SRVs + depth, writes HDR scene color.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"

namespace RG { class RenderContext; }

class LightingPass : public RG::RenderPass
{
public:
    LightingPass(RG::RGTextureHandle albedo,
                 RG::RGTextureHandle normal,
                 RG::RGTextureHandle surface,
                 RG::RGTextureHandle depth,
                 RG::RGTextureHandle emissive);

    const char* GetName() const override { return "LightingPass"; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx)  override;

    // ===== Per-frame setters (called by Renderer before each Execute) =====
    // Pass 0 to disable any individual contribution.

    // IBL cubemaps (irradiance + radiance) and BRDF LUT.
    void SetIBL(uint64_t irradianceHandle, uint64_t radianceHandle,
                uint32_t radianceMips, float iblStrength)
    {
        m_iblIrradianceHandle = irradianceHandle;
        m_iblRadianceHandle   = radianceHandle;
        m_iblRadianceMips     = radianceMips;
        m_iblStrength         = iblStrength;
    }
    void SetBRDFLUT(uint64_t lutHandle) { m_brdfLutHandle = lutHandle; }

    // Shadows: CSM cascade table + spot-shadow atlas/VPs.
    void SetShadowMap(uint64_t shadowHandle) { m_shadowSrvHandle = shadowHandle; }
    void SetSpotShadowAtlas(uint64_t atlasHandle, uint64_t vpsHandle)
    {
        m_spotShadowAtlasHandle = atlasHandle;
        m_spotShadowVPHandle    = vpsHandle;
    }

    // Clustered lighting (all 3 must be non-zero to take effect).
    void SetClusterSRVs(uint64_t lights, uint64_t indexList, uint64_t grid)
    {
        m_clusterLightsSRV    = lights;
        m_clusterIndexListSRV = indexList;
        m_clusterGridSRV      = grid;
    }

    // NPR ramp texture (t16) + per-frame MaterialBuffer SRV.
    void SetRampTexture(uint64_t handle)    { m_rampTexHandle     = handle; }
    void SetMaterialBuffer(uint64_t handle) { m_materialBufHandle = handle; }

    // XeGTAO SSAO SRV (t18); 0 → bind 1×1 white fallback.
    void SetSSAOHandle(uint64_t handle) { m_ssaoSrvHandle = handle; }

    // Sky SH (t19, 9×float4). When valid, shader uses SH for diffuse IBL.
    void SetSkySH(uint64_t handle, bool valid)
    {
        m_skySHHandle = handle;
        m_skySHValid  = valid;
    }

    // Aerial Perspective 3D LUT (t20). Shader gates on aerialMaxDistKm > 0.
    void SetAerialPerspective(uint64_t handle, float maxDistKm)
    {
        m_apHandle    = handle;
        m_apMaxDistKm = maxDistKm;
    }

    // Reflection probe pool (t23/t24) + per-cluster probe list (t25/t26).
    void SetReflectionProbes(uint64_t arrayHandle, uint64_t bufferHandle)
    {
        m_reflectionProbeArrayHandle  = arrayHandle;
        m_reflectionProbeBufferHandle = bufferHandle;
    }
    void SetReflectionProbeCluster(uint64_t gridHandle, uint64_t indexHandle)
    {
        m_reflectionProbeGridHandle  = gridHandle;
        m_reflectionProbeIndexHandle = indexHandle;
    }

    // SSR trace result (t27). 1-frame latent — read from previous frame to
    // dampen specIBL by (1 - ssrConf) before the composite step adds it back.
    void SetSSRResult(uint64_t handle) { m_ssrResultHandle = handle; }

    // DDGI multi-volume bindings — all 4 must be non-zero to contribute.
    // probeSH/depth/probeData are kMaxVolumes-element SRV table bases.
    void SetDDGI(uint64_t volumeBuf, uint64_t probeSHTable,
                 uint64_t depthTable, uint64_t probeDataTable)
    {
        m_ddgiVolumeBufHandle      = volumeBuf;
        m_ddgiProbeSHTableHandle   = probeSHTable;
        m_ddgiDepthTableHandle     = depthTable;
        m_ddgiProbeDataTableHandle = probeDataTable;
    }

private:
    enum class Variant : uint8_t { PBR, NPR, Unlit };
    PSODesc BuildPSODesc(Variant variant) const;

    // ===== Core =====
    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;
    int                 m_sampler       = -1;
    int                 m_iblSampler    = -1;
    int                 m_shadowSampler = -1;     // SamplerComparisonState at s2

    // ===== GBuffer / depth inputs (constructor params) =====
    RG::RGTextureHandle m_albedo;
    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_emissive;

    // ===== Per-frame SRV handles (set by Renderer) =====
    // IBL
    uint64_t m_iblIrradianceHandle = 0;
    uint64_t m_iblRadianceHandle   = 0;
    uint64_t m_brdfLutHandle       = 0;
    uint32_t m_iblRadianceMips     = 7;
    float    m_iblStrength         = 1.0f;
    // Shadows
    uint64_t m_shadowSrvHandle       = 0;
    uint64_t m_spotShadowAtlasHandle = 0;
    uint64_t m_spotShadowVPHandle    = 0;
    // Clustered lighting
    uint64_t m_clusterLightsSRV    = 0;            // t10 space0
    uint64_t m_clusterIndexListSRV = 0;            // t11 space0
    uint64_t m_clusterGridSRV      = 0;            // t12 space0
    // NPR + material
    uint64_t m_rampTexHandle    = 0;               // t16 space0
    uint64_t m_materialBufHandle = 0;              // t12 space0 (per-material NPR params)
    // SSAO + sky
    uint64_t m_ssaoSrvHandle = 0;                  // t18 space0
    uint64_t m_skySHHandle   = 0;                  // t19 space0
    bool     m_skySHValid    = false;
    // Aerial perspective
    uint64_t m_apHandle     = 0;                   // t20 space0
    float    m_apMaxDistKm  = 32.0f;
    // Reflection probes
    uint64_t m_reflectionProbeArrayHandle  = 0;
    uint64_t m_reflectionProbeBufferHandle = 0;
    uint64_t m_reflectionProbeGridHandle   = 0;
    uint64_t m_reflectionProbeIndexHandle  = 0;
    // SSR (1-frame latent)
    uint64_t m_ssrResultHandle = 0;
    // DDGI — base handles into kMaxVolumes-element SRV tables.
    uint64_t m_ddgiVolumeBufHandle      = 0;
    uint64_t m_ddgiProbeSHTableHandle   = 0;
    uint64_t m_ddgiDepthTableHandle     = 0;
    uint64_t m_ddgiProbeDataTableHandle = 0;

    // ===== Init-time fallback resources (root sig must always be satisfied) =====
    // SSAO 1×1 white (reads as 1.0 = no occlusion).
    RHI::Texture m_ssaoFallbackTex;
    uint64_t     m_ssaoFallbackHandle = 0;
    // Sky SH 9×float4 zeros (shader gates reads on iblUseSH).
    RHI::GPUBuffer m_skySHFallback;
    uint64_t       m_skySHFallbackHandle = 0;
    // Aerial Perspective 1×1×1 fallback (zeros).
    RHI::Texture m_apFallbackTex;
    uint64_t     m_apFallbackHandle = 0;
    // Spot-shadow atlas 1×1 (1.0 = fully lit) + identity-VP buffer.
    RHI::Texture   m_spotShadowAtlasFallbackTex;
    uint64_t       m_spotShadowAtlasFallback = 0;
    RHI::GPUBuffer m_spotShadowVPFallbackBuf;
    uint64_t       m_spotShadowVPFallback = 0;
    // NPR ramp 1×1 white (lets NPR_COLOR materials run NPR PSO without a ramp).
    RHI::Texture m_rampFallbackTex;
    uint64_t     m_rampFallbackHandle = 0;

    // Default white fallback (currently unused but retained for future bindless slots).
    uint64_t m_defaultWhiteGpuHandle = 0;
};
