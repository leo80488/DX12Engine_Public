#include "RenderGraph/RenderPass/LightingPass.h"
#include "Graphics/IGraphicsDevice.h"
#include <cstring>
#include "System/Log.h"

// ===== Root parameter slots (must match Lighting.ps.hlsl + GraphicsDX12 root sig) =====
static constexpr uint32_t kIBLIrradianceSlot         = 19;  // t6  space0
static constexpr uint32_t kIBLRadianceSlot           = 20;  // t7  space0
static constexpr uint32_t kBRDFLUTSlot               = 21;  // t8  space0
static constexpr uint32_t kShadowSRVSlot             = 22;  // t9-t11 space0 (CSM table)
static constexpr uint32_t kClusterLightsSlot         = 23;
static constexpr uint32_t kClusterIndexListSlot      = 24;
static constexpr uint32_t kClusterGridSlot           = 25;
static constexpr uint32_t kRampTexSlot               = 26;  // t16 space0
static constexpr uint32_t kMaterialBufSlot           = 28;  // t12 space0
static constexpr uint32_t kEmissiveSRVSlot           = 29;  // t17 space0
static constexpr uint32_t kSSAOSRVSlot               = 30;  // t18 space0
static constexpr uint32_t kSkySHSRVSlot              = 31;  // t19 space0  StructuredBuffer<float4>
static constexpr uint32_t kAerialPerspSlot           = 32;  // t20 space0  Texture3D<float4>
static constexpr uint32_t kSpotShadowAtlasSlot       = 33;  // t21 space0  Texture2DArray<float>
static constexpr uint32_t kSpotShadowVPSlot          = 34;  // t22 space0  StructuredBuffer<float4x4>
static constexpr uint32_t kReflectionProbeArraySlot  = 37;  // t23 space0  TextureCubeArray<float4>
static constexpr uint32_t kReflectionProbeBufferSlot = 38;  // t24 space0  StructuredBuffer<GPUReflectionProbe>
static constexpr uint32_t kReflectionProbeGridSlot   = 39;  // t25 space0  StructuredBuffer<ProbeGridEntry>
static constexpr uint32_t kReflectionProbeIndexSlot  = 40;  // t26 space0  StructuredBuffer<uint>
static constexpr uint32_t kSSRResultSlot             = 41;  // t27 space0  Texture2D<float4>  (prev frame)
static constexpr uint32_t kDDGIVolumeBufSlot         = 42;  // t28 space0  StructuredBuffer<DDGIVolumeGPU>
static constexpr uint32_t kDDGIProbeSHSlot           = 43;  // t29..t32 space0  StructuredBuffer<DDGIProbeSH>[4]
static constexpr uint32_t kDDGIDepthSlot             = 44;  // t33..t36 space0  Texture2D<float2>[4]
static constexpr uint32_t kDDGIProbeDataSlot         = 45;  // t37..t40 space0  StructuredBuffer<DDGIProbeData>[4]

// Stencil refs — must match GBufferPass writes.
static constexpr uint8_t kStencilPBR   = 1;
static constexpr uint8_t kStencilNPR   = 2;
static constexpr uint8_t kStencilUnlit = 3;

// ===== Construction / RG wiring =====

LightingPass::LightingPass(RG::RGTextureHandle albedo,
                           RG::RGTextureHandle normal,
                           RG::RGTextureHandle surface,
                           RG::RGTextureHandle depth,
                           RG::RGTextureHandle emissive)
    : m_albedo(albedo), m_normal(normal), m_surface(surface), m_depth(depth), m_emissive(emissive)
{}

void LightingPass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_albedo);
    b.ReadSRV(m_normal);
    b.ReadSRV(m_surface);
    b.ReadSRV(m_depth);
    b.ReadSRV(m_emissive);
    // Preserve = bind HDR without clear. GBufferPass already wrote the
    // emissive seed there; LightingPass is additive on top.
    b.SetColorTarget(RG::BuiltinTexture::HdrSceneColorPreserve);
}

// ===== PSO build / shader hot-reload =====

PSODesc LightingPass::BuildPSODesc(Variant variant) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::Lighting_VS;
    desc.psID        = ShaderID::Lighting_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = true;

    // Additive blend onto HdrSceneColor.
    //
    // GBufferPass seeds HdrSceneColor with material emissive (Unreal-style
    // direct emissive write to RT5). LightingPass then ADDS its computed
    // diffuse + specular + ambient on top, producing the final
    //   SceneColor = lighting + emissive
    // without emissive ever passing through the BRDF. Each shading-model
    // variant (PBR / NPR / Unlit) returns just its lit contribution and
    // the blend takes care of merging with the emissive seed.
    desc.bs.render_target[0].blend_enable        = true;
    desc.bs.render_target[0].src_blend           = RHI::Blend::ONE;
    desc.bs.render_target[0].dest_blend          = RHI::Blend::ONE;
    desc.bs.render_target[0].blend_op            = RHI::BlendOp::ADD;
    desc.bs.render_target[0].src_blend_alpha     = RHI::Blend::ONE;
    desc.bs.render_target[0].dest_blend_alpha    = RHI::Blend::ONE;
    desc.bs.render_target[0].blend_op_alpha      = RHI::BlendOp::ADD;
    desc.bs.render_target[0].render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;

    // Fullscreen triangle: depth off; stencil EQUAL gates by material type.
    desc.dss.depth_enable       = false;
    desc.dss.depth_write_mask   = RHI::DepthWriteMask::ZERO;
    desc.dss.stencil_enable     = true;
    desc.dss.stencil_read_mask  = 0xFF;
    desc.dss.stencil_write_mask = 0x00;
    desc.dss.front_face.stencil_func    = RHI::ComparisonFunc::EQUAL;
    desc.dss.front_face.stencil_pass_op = RHI::StencilOp::KEEP;
    desc.dss.back_face = desc.dss.front_face;

    switch (variant)
    {
    case Variant::NPR:   desc.perm.Set(PermutationKey::NPR_STENCIL, true); break;
    case Variant::Unlit: desc.perm.Set(PermutationKey::UNLIT,       true); break;
    case Variant::PBR: default: break;
    }
    return desc;
}

void LightingPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::PBR)))
        LOG_ERROR("LightingPass: PBR PSO rebuild after shader reload failed");
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::NPR)))
        LOG_ERROR("LightingPass: NPR PSO rebuild after shader reload failed");
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::Unlit)))
        LOG_ERROR("LightingPass: Unlit PSO rebuild after shader reload failed");
}

// ===== Init: samplers, shader registration, PSO compile, fallback resources =====

void LightingPass::Init(IGraphicsDevice& gfx)
{
    // Linear-clamp sampler for GBuffer / NPR ramp (linear == point at texel centers for 1:1 reads).
    {
        RHI::SamplerDesc sd;
        sd.filter    = RHI::Filter::MIN_MAG_LINEAR_MIP_POINT;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        gfx.CreateSampler(sd, m_sampler);
    }
    // Trilinear-wrap sampler for IBL cubemaps.
    {
        RHI::SamplerDesc sd;
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::WRAP;
        sd.address_v = RHI::TextureAddressMode::WRAP;
        sd.address_w = RHI::TextureAddressMode::WRAP;
        gfx.CreateSampler(sd, m_iblSampler);
    }
    // PCF comparison sampler for shadow maps at s2 (reversed-Z → GREATER_EQUAL).
    {
        RHI::SamplerDesc sd;
        sd.filter          = RHI::Filter::COMPARISON_MIN_MAG_MIP_LINEAR;
        sd.address_u       = RHI::TextureAddressMode::BORDER;
        sd.address_v       = RHI::TextureAddressMode::BORDER;
        sd.address_w       = RHI::TextureAddressMode::BORDER;
        sd.border_color    = RHI::SamplerBorderColor::OPAQUE_WHITE;
        sd.comparison_func = RHI::ComparisonFunc::GREATER_EQUAL;
        if (!gfx.CreateSampler(sd, m_shadowSampler))
            LOG_ERROR("LightingPass: shadow comparison sampler creation failed");
    }

    // ---- Shaders + PSOs ----
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Lighting_VS, RHI::ShaderStage::VS, "Lighting.vs.hlsl");
    m_shaderLib.Register(ShaderID::Lighting_PS, RHI::ShaderStage::PS, "Lighting.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::PBR)))
        LOG_ERROR("LightingPass: PBR PSO creation failed");
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::NPR)))
        LOG_ERROR("LightingPass: NPR PSO creation failed");
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::Unlit)))
        LOG_ERROR("LightingPass: Unlit PSO creation failed");
    else
        LOG_INFO("LightingPass: PBR + NPR + Unlit PSOs ready");

    // ---- Fallback resources (root sig requires every slot bound every frame) ----
    // SSAO 1×1 white → reads as 1.0 = no occlusion.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint8_t white = 0xFF;
        RHI::SubresourceData init{ &white, 1, 1 };
        if (gfx.CreateTexture(td, m_ssaoFallbackTex, &init))
            m_ssaoFallbackHandle = gfx.GetTextureSRVGpuHandle(m_ssaoFallbackTex);
    }
    // Sky SH 9×float4 zeros (shader gates reads on iblUseSH).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = 9 * 4 * sizeof(float);
        bd.stride     = 4 * sizeof(float);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        float zeros[9 * 4] = {};
        if (gfx.CreateBuffer(bd, m_skySHFallback, zeros))
            m_skySHFallbackHandle = gfx.GetBufferSRVGpuHandle(m_skySHFallback);
        else
            LOG_ERROR("LightingPass: SH fallback buffer creation failed");
    }
    // Spot-shadow atlas 1×1 (1.0 = fully lit) — content is moot since shader
    // only samples when shadowSliceIdx != 0xFFFFFFFF.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R32_FLOAT;
        td.width      = 1; td.height = 1;
        td.array_size = 1;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const float one = 1.0f;
        RHI::SubresourceData init{ &one, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(td, m_spotShadowAtlasFallbackTex, &init))
            m_spotShadowAtlasFallback = gfx.GetTextureSRVGpuHandle(m_spotShadowAtlasFallbackTex);
    }
    // Single-identity VP buffer for the spot-shadow VP fallback.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(float) * 16;
        bd.stride     = sizeof(float) * 16;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        if (gfx.CreateBuffer(bd, m_spotShadowVPFallbackBuf, identity))
            m_spotShadowVPFallback = gfx.GetBufferSRVGpuHandle(m_spotShadowVPFallbackBuf);
    }
    // NPR ramp 1×1 white — NPR_COLOR materials need a valid Texture2D<float4> at t16.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        RHI::SubresourceData init{ white, 4, 4 };
        if (gfx.CreateTexture(td, m_rampFallbackTex, &init))
            m_rampFallbackHandle = gfx.GetTextureSRVGpuHandle(m_rampFallbackTex);
    }
    // No AP fallback: SkyIBLPass creates the real 3D LUT and shader gates on aerialMaxDistKm > 0.
}

// ===== Execute: bind GBuffer SRVs + per-frame handles, dispatch 3 stencil-gated PSOs =====

RHI::CommandList LightingPass::Execute(RHI::CommandList cl)
{
    IGraphicsDevice& gfx = cl.GetDevice();

    // Rebind HDR RTV + depth DSV (needed for stencil test).
    if (const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth))
        cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl);

    auto bindResources = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindCBByName(0, "LightCB");
        cl.BindSRVByHandle(0, m_albedo);
        cl.BindSRVByHandle(1, m_normal);
        cl.BindSRVByHandle(2, m_surface);
        cl.BindSRVByHandle(3, m_depth);
        cl.BindSampler(0, m_sampler);
        if (m_iblSampler    >= 0) cl.BindSampler(1, m_iblSampler);
        if (m_shadowSampler >= 0) cl.BindSampler(2, m_shadowSampler);

        // ---- IBL + shadow CSM ----
        if (m_iblIrradianceHandle) cl.BindDescriptorTableHandle(kIBLIrradianceSlot, m_iblIrradianceHandle);
        if (m_iblRadianceHandle)   cl.BindDescriptorTableHandle(kIBLRadianceSlot,   m_iblRadianceHandle);
        if (m_brdfLutHandle)       cl.BindDescriptorTableHandle(kBRDFLUTSlot,       m_brdfLutHandle);
        if (m_shadowSrvHandle)     cl.BindDescriptorTableHandle(kShadowSRVSlot,     m_shadowSrvHandle);

        // ---- Clustered lighting ----
        if (m_clusterLightsSRV)    cl.BindDescriptorTableHandle(kClusterLightsSlot,    m_clusterLightsSRV);
        if (m_clusterIndexListSRV) cl.BindDescriptorTableHandle(kClusterIndexListSlot, m_clusterIndexListSRV);
        if (m_clusterGridSRV)      cl.BindDescriptorTableHandle(kClusterGridSlot,      m_clusterGridSRV);

        // ---- NPR ramp + per-material buffer ----
        // Bind real ramp if supplied, else 1×1 white so NPR_COLOR can run NPR PSO.
        if (m_rampTexHandle)        cl.BindDescriptorTableHandle(kRampTexSlot, m_rampTexHandle);
        else if (m_rampFallbackHandle) cl.BindDescriptorTableHandle(kRampTexSlot, m_rampFallbackHandle);
        if (m_materialBufHandle)    cl.BindDescriptorTableHandle(kMaterialBufSlot, m_materialBufHandle);

        // ---- Emissive GBuffer (t17) ----
        if (const RHI::Texture* emTex = cl.GetContext().GetTexture(m_emissive); emTex && emTex->IsValid())
        {
            if (uint64_t emGpu = cl.GetDevice().GetTextureSRVGpuHandle(*emTex))
                cl.BindDescriptorTableHandle(kEmissiveSRVSlot, emGpu);
        }

        // ---- SSAO (white fallback when disabled → ssao=1.0) ----
        if (m_ssaoSrvHandle)        cl.BindDescriptorTableHandle(kSSAOSRVSlot, m_ssaoSrvHandle);
        else if (m_ssaoFallbackHandle) cl.BindDescriptorTableHandle(kSSAOSRVSlot, m_ssaoFallbackHandle);

        // ---- Sky SH (always bind something; LightCB.iblUseSH gates the read) ----
        if (m_skySHValid && m_skySHHandle) cl.BindDescriptorTableHandle(kSkySHSRVSlot, m_skySHHandle);
        else if (m_skySHFallbackHandle)    cl.BindDescriptorTableHandle(kSkySHSRVSlot, m_skySHFallbackHandle);

        // ---- Aerial Perspective ----
        if (m_apHandle)        cl.BindDescriptorTableHandle(kAerialPerspSlot, m_apHandle);
        else if (m_apFallbackHandle) cl.BindDescriptorTableHandle(kAerialPerspSlot, m_apFallbackHandle);

        // ---- Spot-shadow atlas + per-slice VPs ----
        cl.BindDescriptorTableHandle(kSpotShadowAtlasSlot,
            m_spotShadowAtlasHandle ? m_spotShadowAtlasHandle : m_spotShadowAtlasFallback);
        cl.BindDescriptorTableHandle(kSpotShadowVPSlot,
            m_spotShadowVPHandle    ? m_spotShadowVPHandle    : m_spotShadowVPFallback);

        // ---- Reflection probes (probeArray doubles as fallback for the other 3) ----
        cl.BindDescriptorTableHandle(kReflectionProbeArraySlot,  m_reflectionProbeArrayHandle);
        cl.BindDescriptorTableHandle(kReflectionProbeBufferSlot,
            m_reflectionProbeBufferHandle ? m_reflectionProbeBufferHandle : m_reflectionProbeArrayHandle);
        cl.BindDescriptorTableHandle(kReflectionProbeGridSlot,
            m_reflectionProbeGridHandle   ? m_reflectionProbeGridHandle   : m_reflectionProbeArrayHandle);
        cl.BindDescriptorTableHandle(kReflectionProbeIndexSlot,
            m_reflectionProbeIndexHandle  ? m_reflectionProbeIndexHandle  : m_reflectionProbeArrayHandle);

        // ---- SSR (1-frame latent; ssrConf==0 path inert when handle missing) ----
        cl.BindDescriptorTableHandle(kSSRResultSlot,
            m_ssrResultHandle ? m_ssrResultHandle : m_reflectionProbeArrayHandle);

        // ---- DDGI (volume buf + 3 multi-volume tables; reuse probe array as null fallback) ----
        cl.BindDescriptorTableHandle(kDDGIVolumeBufSlot,
            m_ddgiVolumeBufHandle      ? m_ddgiVolumeBufHandle      : m_reflectionProbeArrayHandle);
        cl.BindDescriptorTableHandle(kDDGIProbeSHSlot,
            m_ddgiProbeSHTableHandle   ? m_ddgiProbeSHTableHandle   : m_reflectionProbeArrayHandle);
        cl.BindDescriptorTableHandle(kDDGIDepthSlot,
            m_ddgiDepthTableHandle     ? m_ddgiDepthTableHandle     : m_reflectionProbeArrayHandle);
        cl.BindDescriptorTableHandle(kDDGIProbeDataSlot,
            m_ddgiProbeDataTableHandle ? m_ddgiProbeDataTableHandle : m_reflectionProbeArrayHandle);
    };

    auto draw = [&](Variant variant, uint8_t stencilRef)
    {
        const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc(variant));
        if (!pso || !pso->IsValid()) return;
        cl.SetPipelineState(*pso);
        bindResources();
        gfx.SetStencilRef(stencilRef, cl);
        cl.DrawFullscreenTriangle();
    };

    draw(Variant::PBR, kStencilPBR);
    // NPR pass runs whenever a real ramp OR the fallback is bound (NPR_COLOR
    // materials don't sample the ramp but still need a valid t16).
    if (m_rampTexHandle || m_rampFallbackHandle)
        draw(Variant::NPR, kStencilNPR);
    draw(Variant::Unlit, kStencilUnlit);

    return cl;
}
