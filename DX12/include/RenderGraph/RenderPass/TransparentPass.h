#pragma once

// TransparentPass — forward PBR pass for alpha-blended geometry.
//
// Renders after GBufferPass + LightingPass + SkyboxPass onto the same HDR target,
// using the GBuffer depth buffer for occlusion (depth-test ON, depth-write OFF).
//
// Draw list: DrawFilter::Transparent packets, pre-sorted back-to-front
//            (painter's algorithm) by Renderer::BuildRenderScene.
//
// Blend modes are encoded in the PermutationKey:
//   ALPHA_BLEND=1, ADDITIVE_BLEND=0, PREMULTIPLIED_BLEND=0  → SRC_ALPHA/INV_SRC_ALPHA
//   ALPHA_BLEND=1, ADDITIVE_BLEND=1                         → ONE/ONE  (additive)
//   ALPHA_BLEND=1, PREMULTIPLIED_BLEND=1                    → ONE/INV_SRC_ALPHA
//
// Shader bindings (LightCB at b2 — VS owns b1 for PerViewCB):
//   See Transparent.ps.hlsl for full register table.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"

class TransparentPass : public RG::RenderPass
{
public:
    explicit TransparentPass(RG::RGTextureHandle depth);

    const char* GetName() const override { return "TransparentPass"; }
    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // IBL handles forwarded from Renderer::SyncSkyboxIBL each frame.
    void SetIBL(uint64_t irradiance, uint64_t radiance,
                uint32_t radianceMips, float strength)
    {
        m_iblIrradianceHandle = irradiance;
        m_iblRadianceHandle   = radiance;
        (void)radianceMips; (void)strength; // carried by LightCB
    }
    void SetBRDFLUT(uint64_t lut) { m_brdfLutHandle = lut; }

    /** Sky SH StructuredBuffer SRV (t19 space0, 9 × float4) — same buffer the
     *  deferred LightingPass reads. When `valid` is true and LightCB.iblUseSH=1
     *  the forward shader uses SH evaluation for diffuse IBL instead of
     *  sampling the static irradiance cubemap. */
    void SetSkySH(uint64_t handle) { m_skySHHandle = handle; }

    /** Reflection probe pool bindings (t23 / t24 space0). Mirrors
     *  LightingPass::SetReflectionProbes — root sig requires the array SRV
     *  to always be bound; the shader's `reflectionProbeCount == 0` early-
     *  exit guards reads when no probes are active this frame. */
    void SetReflectionProbes(uint64_t arrayHandle, uint64_t bufferHandle)
    {
        m_reflectionProbeArrayHandle  = arrayHandle;
        m_reflectionProbeBufferHandle = bufferHandle;
    }

    // Global view-mode: FILL_MODE_WIREFRAME when true (baked into the PSO key).
    // Driven per-frame by Renderer from ViewMode::Wireframe. The forward shader
    // (Transparent.ps) handles the Unlit/Wireframe color itself.
    void SetWireframe(bool w) { m_wireframeMode = w; }

private:
    // Build PSO descriptor for a given permutation (selects blend mode + cull).
    // `customPSID > 0` substitutes the default Transparent_PS for a dynamic
    // shader registered via ShaderLibrary::RegisterDynamic — same convention
    // GBufferPass uses for material custom shaders, so a material with
    // `useCustomShader=true + userBlendMode=Additive` flows naturally to
    // this pass.
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customPSID = 0) const;

    RG::RGTextureHandle m_depth;

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    int  m_linearSamplerIdx = -1;   // s0: material texture sampler
    int  m_iblSamplerIdx    = -1;   // s1: IBL trilinear-wrap sampler

    // Default fallback textures (shared with GBufferPass logic)
    RHI::Texture m_defaultWhite;
    uint64_t     m_defaultWhiteGpuHandle = 0;
    RHI::Texture m_defaultFlatNormal;
    uint64_t     m_defaultFlatNormalGpuHandle = 0;

    // IBL handles — set each frame by Renderer.
    uint64_t m_iblIrradianceHandle = 0;
    uint64_t m_iblRadianceHandle   = 0;
    uint64_t m_brdfLutHandle       = 0;

    // Sky SH (t19 space0) — set by Renderer::SyncSkyboxIBL each frame.
    // Empty handle is acceptable: the shader gates on LightCB.iblUseSH
    // which is also written from Renderer (0 when SkyIBLPass hasn't run).
    uint64_t m_skySHHandle = 0;

    // Reflection probe pool (t23 / t24 space0) — wired once at Compile time.
    // The handles point at persistent resources owned by ReflectionProbeManager.
    uint64_t m_reflectionProbeArrayHandle  = 0;
    uint64_t m_reflectionProbeBufferHandle = 0;

    // Global view-mode wireframe toggle (see SetWireframe).
    bool     m_wireframeMode = false;
};
