#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

// Compute root sig (shared with all the other compute passes — see
// CreateComputeRootSignature in GraphicsDX12.cpp).
static constexpr uint32_t kCBSlot   = 0;   // b0 space2
static constexpr uint32_t kSRVT0    = 1;   // t0 space2
static constexpr uint32_t kSRVT1    = 2;   // t1 space2
static constexpr uint32_t kCBSlotB1 = 6;   // b1 space2 (using a CBV table slot)
                                          // (re-uses the SRV t5 slot since
                                          //  CSM matrices land via direct CBV.
                                          //  Light-inject shader binds CSM
                                          //  matrices as a 2nd CBV at b1.)
static constexpr uint32_t kUAV0     = 4;   // u0 space2

// Apply pass uses the GRAPHICS root sig — slot indices match LightingPass.
static constexpr uint32_t kEnvMapRootSlot = 19;  // t6 space0 — we re-purpose it
                                                 // to bind the 3D scattering
                                                 // SRV for the apply PS.

// FroxelConstants / VolApplyConstants moved to VolumetricFogPass.h so the
// per-frame CBs can be FrameCB<T> members (T needs to be a complete type at
// the FrameCB<T> instantiation point).

// ---------------------------------------------------------------------------
VolumetricFogPass::VolumetricFogPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{
    DirectX::XMStoreFloat4x4(&m_viewProj,     DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&m_invViewProj,  DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&m_prevViewProj, DirectX::XMMatrixIdentity());
    for (int i = 0; i < 3; ++i)
        DirectX::XMStoreFloat4x4(&m_shadowMatrix[i], DirectX::XMMatrixIdentity());
}

VolumetricFogPass::~VolumetricFogPass()
{
    if (!m_gfx) return;
    if (m_densityTex.IsValid())    m_gfx->DestroyTexture(m_densityTex);
    if (m_lightingTex.IsValid())   m_gfx->DestroyTexture(m_lightingTex);
    if (m_scatteringTex.IsValid()) m_gfx->DestroyTexture(m_scatteringTex);
    for (int i = 0; i < 2; ++i)
        if (m_historyTex[i].IsValid()) m_gfx->DestroyTexture(m_historyTex[i]);
    DestroyRaymarchTextures();
    m_paramCB.Destroy(*m_gfx);
    m_applyCB.Destroy(*m_gfx);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_volLightsBufMapped[i])    m_gfx->UnmapBuffer(m_volLightsBuf[i]);
        if (m_volLightsBuf[i].IsValid()) m_gfx->DestroyBuffer(m_volLightsBuf[i]);
        m_volLightsBufMapped[i] = nullptr;
        m_lightsSrv[i]          = 0;
    }
}

void VolumetricFogPass::Setup(RG::RenderGraphBuilder& b)
{
    // Graph-managed depth declared as SRV → graph emits the
    // DEPTHSTENCIL → DEPTH_READ_SRV barrier before this pass executes.
    //
    // IMPORTANT: do NOT declare HdrSceneColor as the colour target. The graph
    // would CLEAR it on entry — when the pass is disabled (Execute early-out)
    // we'd end up with a cleared HDR (black) and no draws to fill it back in.
    // Instead bind HDR manually via SetRenderTargetToHdrWithDepth() in
    // DrawApply (same pattern as SkyboxPass).
    b.ReadSRV(m_depth);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void VolumetricFogPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Shader registration + PSO compile ----------------------------------
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::FroxelDensity_CS,     RHI::ShaderStage::CS,
                         "FroxelDensity.cs.hlsl",     "main");
    m_shaderLib.Register(ShaderID::FroxelLightInject_CS, RHI::ShaderStage::CS,
                         "FroxelLightInject.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::FroxelScatter_CS,     RHI::ShaderStage::CS,
                         "FroxelScatter.cs.hlsl",     "main");
    m_shaderLib.Register(ShaderID::FroxelTemporal_CS,    RHI::ShaderStage::CS,
                         "FroxelTemporal.cs.hlsl",    "main");
    m_shaderLib.Register(ShaderID::VolumetricApply_VS,   RHI::ShaderStage::VS,
                         "VolumetricApply.vs.hlsl",   "main");
    m_shaderLib.Register(ShaderID::VolumetricApply_PS,   RHI::ShaderStage::PS,
                         "VolumetricApply.ps.hlsl",   "main");
    m_shaderLib.Register(ShaderID::VolumetricRaymarch_CS,
                         RHI::ShaderStage::CS,
                         "VolumetricRaymarch.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::VolumetricRaymarchTemporal_CS,
                         RHI::ShaderStage::CS,
                         "VolumetricRaymarchTemporal.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::VolumetricRaymarchApply_PS,
                         RHI::ShaderStage::PS,
                         "VolumetricRaymarchApply.ps.hlsl", "main");

    auto makeCs = [&](ShaderID id, RHI::PipelineState& out, const char* tag)
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("VolumetricFogPass: %s_CS not found", tag); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, out))
            LOG_ERROR("VolumetricFogPass: %s PSO failed", tag);
    };
    makeCs(ShaderID::FroxelDensity_CS,             m_densityPSO,       "Density");
    makeCs(ShaderID::FroxelLightInject_CS,         m_lightInjectPSO,   "LightInject");
    makeCs(ShaderID::FroxelScatter_CS,             m_scatterPSO,       "Scatter");
    makeCs(ShaderID::FroxelTemporal_CS,            m_temporalPSO,      "Temporal");
    makeCs(ShaderID::VolumetricRaymarch_CS,        m_rmPSO,            "Raymarch");
    makeCs(ShaderID::VolumetricRaymarchTemporal_CS,m_rmTemporalPSO,    "RaymarchTemporal");

    // Apply graphics PSO via PSOCache (will use the engine's global root sig).
    m_psoCache.Init(gfx, m_shaderLib);
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VolumetricApply_VS;
        desc.psID        = ShaderID::VolumetricApply_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cull_mode         = RHI::CullMode::NONE;
        desc.rs.depth_clip_enable = false;
        // Composite over HDR using premultiplied scattering and (1-T) alpha.
        desc.dss.depth_enable     = false;
        desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
        auto& bs = desc.bs.render_target[0];
        bs.blend_enable           = true;
        bs.src_blend              = RHI::Blend::ONE;
        bs.dest_blend             = RHI::Blend::INV_SRC_ALPHA;
        bs.blend_op               = RHI::BlendOp::ADD;
        bs.src_blend_alpha        = RHI::Blend::ZERO;
        bs.dest_blend_alpha       = RHI::Blend::ONE;
        bs.blend_op_alpha         = RHI::BlendOp::ADD;
        bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;

        if (!m_psoCache.GetOrCreate(desc))
            LOG_ERROR("VolumetricFogPass: apply PSO failed");
    }

    // Raymarch apply graphics PSO — additive blend (src=ONE, dst=ONE). The
    // froxel VolumetricApply downstream handles medium transmittance, so the
    // raymarch composite only contributes in-scatter radiance and must not
    // modify the HDR alpha or re-apply extinction.
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VolumetricApply_VS;
        desc.psID        = ShaderID::VolumetricRaymarchApply_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cull_mode         = RHI::CullMode::NONE;
        desc.rs.depth_clip_enable = false;
        desc.dss.depth_enable     = false;
        desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
        auto& bs = desc.bs.render_target[0];
        bs.blend_enable           = true;
        bs.src_blend              = RHI::Blend::ONE;
        bs.dest_blend             = RHI::Blend::ONE;
        bs.blend_op               = RHI::BlendOp::ADD;
        bs.src_blend_alpha        = RHI::Blend::ZERO;
        bs.dest_blend_alpha       = RHI::Blend::ONE;
        bs.blend_op_alpha         = RHI::BlendOp::ADD;
        bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;

        if (!m_psoCache.GetOrCreate(desc))
            LOG_ERROR("VolumetricFogPass: raymarch apply PSO failed");
    }

    // ---- 3D textures + descriptors -----------------------------------------
    Create3DTextures(gfx);

    // ---- CBs (triple-buffered) --------------------------------------------
    if (!m_paramCB.Create(gfx, "VolumetricFogPass.ParamCB"))
        LOG_ERROR("VolumetricFogPass: ParamCB create failed");
    if (!m_applyCB.Create(gfx, "VolumetricFogPass.ApplyCB"))
        LOG_ERROR("VolumetricFogPass: ApplyCB create failed");

    // Volumetric light StructuredBuffer<GPULight> (UPLOAD), triple-buffered.
    // Holds the subset of scene lights that opted into volumetric
    // (VolumetricLightComponent).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kMaxVolLights * sizeof(VolLight);
        bd.stride     = sizeof(VolLight);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_volLightsBuf[i]))
            {
                m_volLightsBufMapped[i] = gfx.MapBuffer(m_volLightsBuf[i]);
                m_lightsSrv[i]          = gfx.GetBufferSRVGpuHandle(m_volLightsBuf[i]);
            }
        }
    }

    // ---- Samplers ----------------------------------------------------------
    {
        RHI::SamplerDesc sd;
        sd.filter          = RHI::Filter::COMPARISON_MIN_MAG_MIP_LINEAR;
        sd.address_u       = RHI::TextureAddressMode::BORDER;
        sd.address_v       = RHI::TextureAddressMode::BORDER;
        sd.address_w       = RHI::TextureAddressMode::BORDER;
        sd.border_color    = RHI::SamplerBorderColor::OPAQUE_WHITE;
        sd.comparison_func = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z
        gfx.CreateSampler(sd, m_shadowSampler);
    }
    {
        RHI::SamplerDesc sd;
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        gfx.CreateSampler(sd, m_linearSampler);
    }

    LOG_SUCCESS("VolumetricFogPass: initialised (%ux%ux%u)",
                kFroxelW, kFroxelH, kFroxelD);
}

// ---------------------------------------------------------------------------
void VolumetricFogPass::Create3DTextures(IGraphicsDevice& gfx)
{
    auto makeFroxel = [&](RHI::Texture& outTex, const char* tag)
    {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
        td.width      = kFroxelW;
        td.height     = kFroxelH;
        td.depth      = kFroxelD;
        td.mip_levels = 1;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        if (!gfx.CreateTexture(td, outTex))
            LOG_ERROR("VolumetricFogPass: %s 3D texture create failed", tag);
    };

    makeFroxel(m_densityTex,    "Density");
    makeFroxel(m_lightingTex,   "Lighting");
    makeFroxel(m_scatteringTex, "Scattering");

    for (int i = 0; i < 2; ++i)
    {
        char name[24];
        std::snprintf(name, sizeof(name), "History[%d]", i);
        makeFroxel(m_historyTex[i], name);
    }
}

// ---------------------------------------------------------------------------
void VolumetricFogPass::DestroyRaymarchTextures()
{
    if (!m_gfx) return;
    if (m_rmCurrentTex.IsValid())    m_gfx->DestroyTexture(m_rmCurrentTex);
    for (int i = 0; i < 2; ++i)
        if (m_rmHistoryTex[i].IsValid()) m_gfx->DestroyTexture(m_rmHistoryTex[i]);
    m_rmCurrentTex = {};
    m_rmHistoryTex[0] = {};
    m_rmHistoryTex[1] = {};
    m_rmHistoryValid = false;
}

void VolumetricFogPass::RebuildRaymarchTextures(uint32_t fullW, uint32_t fullH)
{
    if (!m_gfx) return;
    DestroyRaymarchTextures();

    // Half-res — matches the 32-step raymarch budget. Floor-div-round-up so
    // odd viewport dims (1921×1081 etc.) still get a clean 2× relationship.
    m_rmHalfW = (fullW + 1) / 2;
    m_rmHalfH = (fullH + 1) / 2;
    if (m_rmHalfW == 0 || m_rmHalfH == 0) return;

    auto make2D = [&](RHI::Texture& outTex, const char* tag)
    {
        RHI::TextureDesc td{};
        td.width      = m_rmHalfW;
        td.height     = m_rmHalfH;
        td.depth      = 1;
        td.mip_levels = 1;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!m_gfx->CreateTexture(td, outTex))
            LOG_ERROR("VolumetricFogPass: %s half-res texture create failed", tag);
    };
    make2D(m_rmCurrentTex,     "RM.Current");
    make2D(m_rmHistoryTex[0],  "RM.History[0]");
    make2D(m_rmHistoryTex[1],  "RM.History[1]");

    m_rmCurrentState    = RHI::ResourceState::UNORDERED_ACCESS;
    m_rmHistoryState[0] = RHI::ResourceState::UNORDERED_ACCESS;
    m_rmHistoryState[1] = RHI::ResourceState::UNORDERED_ACCESS;
    m_rmLastFullW = fullW;
    m_rmLastFullH = fullH;
    m_rmHistoryValid = false;
    LOG_INFO("VolumetricFogPass: raymarch half-res rebuilt %ux%u", m_rmHalfW, m_rmHalfH);
}

// ---------------------------------------------------------------------------
void VolumetricFogPass::SetVolumetricLights(const std::vector<VolLight>& lights)
{
    m_spotLightCount = static_cast<uint32_t>((std::min)(lights.size(),
                                                        size_t(kMaxVolLights)));
    if (!m_gfx || m_spotLightCount == 0) return;
    const uint32_t s = m_gfx->GetFrameIndex();
    if (s < kFrameCount && m_volLightsBufMapped[s])
    {
        std::memcpy(m_volLightsBufMapped[s], lights.data(),
                    m_spotLightCount * sizeof(VolLight));
    }
}

// ---------------------------------------------------------------------------
void VolumetricFogPass::SetFrameState(const DirectX::XMFLOAT4X4& viewProj,
                                       const DirectX::XMFLOAT4X4& invViewProj,
                                       const DirectX::XMFLOAT4X4& /*prevViewProj*/,
                                       const DirectX::XMFLOAT3&   cameraPos,
                                       float nearPlane, float farPlane,
                                       uint32_t frameIndex)
{
    // IMPORTANT: m_prevViewProj is NOT overwritten by the caller — we snapshot
    // the current viewProj at the END of Execute() so the temporal pass can
    // reproject against a guaranteed-correct previous-frame matrix regardless
    // of whether TAA is on (which was the previous source of prevVP).
    //
    // First-frame bootstrap: without this seed prevViewProj is identity, every
    // voxel's reprojected UV is out of bounds, the temporal pass takes the
    // `HistoryUAV = current` early exit for every cell, and the history buffer
    // is populated with this frame's *un-blended* values. That's wrong for the
    // next frame's blend (it looks like a single-frame snapshot instead of a
    // proper running average). Seeding prevViewProj with the current VP makes
    // the reprojection a no-op on frame 0 and the blend becomes well-defined.
    if (!m_firstFrameDone)
    {
        m_prevViewProj   = viewProj;
        m_prevCameraPos  = cameraPos;
        m_firstFrameDone = true;
    }
    m_viewProj      = viewProj;
    m_invViewProj   = invViewProj;
    m_cameraPos     = cameraPos;
    m_nearPlane     = nearPlane;
    m_farPlane      = farPlane;
    m_frameIndex    = frameIndex;
}

void VolumetricFogPass::SetShadowState(uint64_t shadowSrvHandle,
                                       const DirectX::XMFLOAT4X4 shadowMatrix[3],
                                       const DirectX::XMFLOAT3& cascadeSplits,
                                       float texelSize, float bias, float strength)
{
    m_shadowSrv         = shadowSrvHandle;
    for (int i = 0; i < 3; ++i) m_shadowMatrix[i] = shadowMatrix[i];
    m_cascadeSplits     = cascadeSplits;
    m_shadowTexelSize   = texelSize;
    m_shadowBias        = bias;
    m_shadowStrength    = strength;
}

// ---------------------------------------------------------------------------
static void TransitionTex(IGraphicsDevice& gfx,
                          const RHI::Texture& tex,
                          RHI::ResourceState& state,
                          RHI::ResourceState to,
                          RHI::CommandList cl)
{
    if (state == to) return;
    gfx.PushBarrier(RHI::GPUBarrier::Image(&tex, state, to), cl);
    state = to;
}

void VolumetricFogPass::DispatchDensity(RHI::CommandList cl)
{
    if (!m_densityPSO.IsValid() || !m_densityTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_densityTex, m_densityState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_densityPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_densityTex), cl);
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, kFroxelD, cl);

    TransitionTex(gfx, m_densityTex, m_densityState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

void VolumetricFogPass::DispatchLightInject(RHI::CommandList cl)
{
    if (!m_lightInjectPSO.IsValid() || !m_lightingTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_lightingTex, m_lightingState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_lightInjectPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRVT0, gfx.GetTextureSRVGpuHandle(m_densityTex), cl);
    // CSM shadow array — ShadowPass now leaves it in DEPTH_READ|PIXEL_SR|
    // NON_PIXEL_SR (DEPTH_READ_SRV in the engine RHI), so the compute path
    // can read it directly. Bind for sun god-rays.
    if (m_shadowSrv)
        gfx.SetComputeDescriptorTable(kSRVT1, m_shadowSrv, cl);
    // Clustered-lighting GPULight buffer (point + spot lights for volumetric).
    {
        const uint64_t lightsSrv = m_lightsSrv[gfx.GetFrameIndex()];
        if (lightsSrv)
            gfx.SetComputeDescriptorTable(/*t2 space2 root param 3*/ 3, lightsSrv, cl);
    }
    // Scene depth (t3 space2, root param 7) — used by ScreenSpaceShadow inside
    // the compute shader so spot/point lights don't beam through walls. The
    // graph's b.ReadSRV(m_depth) declaration in Setup() already transitions
    // the resource into DEPTH_READ_SRV for us, so the compute path can sample
    // it without a manual barrier.
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(/*t3 space2 root param 7*/ 7,
                                      gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    // Scene occupancy (t4 space2, root param 8) — optional; when unbound, the
    // shader falls through to the ScreenSpaceShadow-only visibility path.
    if (m_voxelOccupancySrv)
        gfx.SetComputeDescriptorTable(/*t4 space2 root param 8*/ 8,
                                      m_voxelOccupancySrv, cl);
    // Spot-light shadow atlas + VP matrices (t6 / t7 space2, root params 12/13).
    // Only sampled when a light has shadowSliceIdx != 0xFFFFFFFF — binding is
    // still required because the root sig declares the slots.
    if (m_spotShadowAtlasSrv)
        gfx.SetComputeDescriptorTable(/*t6 space2 root param 12*/ 12,
                                      m_spotShadowAtlasSrv, cl);
    if (m_spotShadowVPSrv)
        gfx.SetComputeDescriptorTable(/*t7 space2 root param 13*/ 13,
                                      m_spotShadowVPSrv, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_lightingTex), cl);
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, kFroxelD, cl);

    TransitionTex(gfx, m_lightingTex, m_lightingState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

void VolumetricFogPass::DispatchScatter(RHI::CommandList cl)
{
    if (!m_scatterPSO.IsValid() || !m_scatteringTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_scatteringTex, m_scatteringState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_scatterPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRVT0, gfx.GetTextureSRVGpuHandle(m_lightingTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0,  gfx.GetTextureUAVGpuHandle(m_scatteringTex), cl);
    // XY only — shader walks Z internally.
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, 1, cl);

    // Land in compute-SR; subsequent stage decides the next state:
    //   - Temporal on  → keeps it as compute-SR (its CS reads it).
    //   - Temporal off → transitions to pixel-SR before Apply samples it.
    TransitionTex(gfx, m_scatteringTex, m_scatteringState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

void VolumetricFogPass::DispatchTemporal(RHI::CommandList cl)
{
    if (!m_temporalPSO.IsValid() || !m_historyTex[0].IsValid() || !m_historyTex[1].IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    const uint32_t r = m_historyReadIdx;
    const uint32_t w = m_historyWriteIdx;

    // Read buffer → SRV (compute), Write buffer → UAV.
    TransitionTex(gfx, m_historyTex[r], m_historyState[r],
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_historyTex[w], m_historyState[w],
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_temporalPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRVT0, gfx.GetTextureSRVGpuHandle(m_scatteringTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT1, gfx.GetTextureSRVGpuHandle(m_historyTex[r]), cl);
    gfx.SetComputeDescriptorTable(kUAV0,  gfx.GetTextureUAVGpuHandle(m_historyTex[w]), cl);
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, kFroxelD, cl);

    // Hand the just-written history off to the apply pixel shader.
    TransitionTex(gfx, m_historyTex[w], m_historyState[w],
                  RHI::ResourceState::SHADER_RESOURCE, cl);

    m_historyValid = true;
}

// ---------------------------------------------------------------------------
void VolumetricFogPass::DispatchRaymarch(RHI::CommandList cl)
{
    if (!m_rmPSO.IsValid() || !m_rmCurrentTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_rmCurrentTex, m_rmCurrentState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_rmPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    // t0 space2 = FroxelDensity 3D (σ_s / σ_t lookup, same as density output).
    gfx.SetComputeDescriptorTable(kSRVT0, gfx.GetTextureSRVGpuHandle(m_densityTex), cl);
    // t1 space2 = CSM array (sun shadowing).
    if (m_shadowSrv)
        gfx.SetComputeDescriptorTable(kSRVT1, m_shadowSrv, cl);
    // t2 space2 = Lights buffer (opted-in volumetric lights).
    {
        const uint64_t lightsSrv = m_lightsSrv[gfx.GetFrameIndex()];
        if (lightsSrv)
            gfx.SetComputeDescriptorTable(3, lightsSrv, cl);
    }
    // t3 space2 = Scene depth (view-Z clamp + sky detection).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(7, gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    // t6 / t7 space2 = SpotShadowPass atlas + per-slice VP buffer.
    if (m_spotShadowAtlasSrv)
        gfx.SetComputeDescriptorTable(12, m_spotShadowAtlasSrv, cl);
    if (m_spotShadowVPSrv)
        gfx.SetComputeDescriptorTable(13, m_spotShadowVPSrv, cl);
    // u0 space2 = half-res in-scatter output.
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(m_rmCurrentTex), cl);

    gfx.DispatchCompute((m_rmHalfW + 7) / 8, (m_rmHalfH + 7) / 8, 1, cl);

    TransitionTex(gfx, m_rmCurrentTex, m_rmCurrentState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

void VolumetricFogPass::DispatchRaymarchTemporal(RHI::CommandList cl)
{
    if (!m_rmTemporalPSO.IsValid() || !m_rmHistoryTex[0].IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    const uint32_t r = m_rmHistoryReadIdx;
    const uint32_t w = m_rmHistoryWriteIdx;

    TransitionTex(gfx, m_rmHistoryTex[r], m_rmHistoryState[r],
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_rmHistoryTex[w], m_rmHistoryState[w],
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_rmTemporalPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRVT0,
        gfx.GetTextureSRVGpuHandle(m_rmCurrentTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT1,
        gfx.GetTextureSRVGpuHandle(m_rmHistoryTex[r]), cl);
    // Scene depth (t3 space2) — the temporal CS reprojects via the surface
    // anchor reconstructed from scene depth, same as the raymarch pass itself.
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(7, gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(m_rmHistoryTex[w]), cl);

    gfx.DispatchCompute((m_rmHalfW + 7) / 8, (m_rmHalfH + 7) / 8, 1, cl);

    TransitionTex(gfx, m_rmHistoryTex[w], m_rmHistoryState[w],
                  RHI::ResourceState::SHADER_RESOURCE, cl);
    m_rmHistoryValid = true;
}

void VolumetricFogPass::DrawRaymarchApply(RHI::CommandList cl)
{
    PSODesc desc;
    desc.vsID        = ShaderID::VolumetricApply_VS;
    desc.psID        = ShaderID::VolumetricRaymarchApply_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = false;
    desc.dss.depth_enable     = false;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    auto& bs = desc.bs.render_target[0];
    bs.blend_enable     = true;
    bs.src_blend        = RHI::Blend::ONE;
    bs.dest_blend       = RHI::Blend::ONE;
    bs.blend_op         = RHI::BlendOp::ADD;
    bs.src_blend_alpha  = RHI::Blend::ZERO;
    bs.dest_blend_alpha = RHI::Blend::ONE;
    bs.blend_op_alpha   = RHI::BlendOp::ADD;
    bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return;

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    cl.BindCBByName(2, "VolApplyCB");
    cl.BindSRVByHandle(3, m_depth);

    // Half-res raymarch source (post-temporal if history is primed). The
    // graphics root sig's t6 space0 slot (env cube) is reused for this 2D
    // SRV, matching the pattern used by the froxel VolumetricApply PS.
    const uint64_t srv = (m_temporalEnabled && m_rmHistoryValid)
        ? m_gfx->GetTextureSRVGpuHandle(m_rmHistoryTex[m_rmHistoryWriteIdx])
        : m_gfx->GetTextureSRVGpuHandle(m_rmCurrentTex);
    cl.BindDescriptorTableHandle(kEnvMapRootSlot, srv);
    if (m_linearSampler >= 0) cl.BindSampler(0, m_linearSampler);

    cl.DrawInstanced(3, 1, 0, 0);
}

void VolumetricFogPass::DrawApply(RHI::CommandList cl)
{
    PSODesc desc;
    desc.vsID        = ShaderID::VolumetricApply_VS;
    desc.psID        = ShaderID::VolumetricApply_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = false;
    desc.dss.depth_enable     = false;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    auto& bs = desc.bs.render_target[0];
    bs.blend_enable     = true;
    bs.src_blend        = RHI::Blend::ONE;
    bs.dest_blend       = RHI::Blend::INV_SRC_ALPHA;
    bs.blend_op         = RHI::BlendOp::ADD;
    bs.src_blend_alpha  = RHI::Blend::ZERO;
    bs.dest_blend_alpha = RHI::Blend::ONE;
    bs.blend_op_alpha   = RHI::BlendOp::ADD;
    bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return;

    // Upload tiny Apply CB (per-frame; near/far + froxel range).
    if (auto* slot = m_applyCB.Current(*m_gfx))
    {
        VolApplyConstants c{};
        c.nearZ        = m_nearPlane;
        c.farZ         = m_farPlane;
        c.froxelNear   = m_froxelNear;
        c.froxelFar    = m_froxelFar;
        c.froxelDepth  = kFroxelD;
        *slot = c;
    }

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    // The Apply PS samples the 3D scattering texture as t6 (re-uses the
    // skybox env-cube root slot — unused by this PS otherwise) and the
    // graph-managed depth at t5. CB lives at b3 space0 (slot 2 in the
    // BindCBByName index).
    cl.BindCBByName(2, "VolApplyCB");
    cl.BindSRVByHandle(3, m_depth);   // SRV slot 3 = root param 13 (t5 space0)
    const uint64_t srv = m_applyOverrideSrv ? m_applyOverrideSrv
                                            : m_gfx->GetTextureSRVGpuHandle(m_scatteringTex);
    cl.BindDescriptorTableHandle(kEnvMapRootSlot, srv);
    if (m_linearSampler >= 0) cl.BindSampler(0, m_linearSampler);

    cl.DrawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
RHI::CommandList VolumetricFogPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled || m_viewModeHidden) return cl;
    if (!m_densityPSO.IsValid()) return cl;

    IGraphicsDevice& gfx = *m_gfx;

    // Rebuild half-res raymarch textures on viewport change.
    if (m_rmEnabled)
    {
        const uint32_t renderW = gfx.GetRenderWidth();
        const uint32_t renderH = gfx.GetRenderHeight();
        if (renderW > 0 && renderH > 0 &&
            (renderW != m_rmLastFullW || renderH != m_rmLastFullH))
        {
            RebuildRaymarchTextures(renderW, renderH);
        }
    }

    // Camera teleport detection — if the camera moved more than a few metres
    // since last frame (scene switch, debug-cam jump, cinematic cut) the
    // reprojected history is almost certainly wrong, and blending it in just
    // produces visible ghosting until the history drifts back. Detect that
    // case and temporarily force alpha = 1 so we take only the current frame.
    const DirectX::XMVECTOR prevP = DirectX::XMLoadFloat3(&m_prevCameraPos);
    const DirectX::XMVECTOR currP = DirectX::XMLoadFloat3(&m_cameraPos);
    const float camDelta = DirectX::XMVectorGetX(
        DirectX::XMVector3Length(DirectX::XMVectorSubtract(currP, prevP)));
    constexpr float kTeleportThreshold = 5.0f;
    const float effectiveAlpha =
        (m_historyValid && m_externalHistoryValid && camDelta < kTeleportThreshold)
            ? m_temporalAlpha : 1.0f;

    // Upload FroxelConstants.
    if (auto* paramCBSlot = m_paramCB.Current(gfx))
    {
        FroxelConstants c{};
        std::memcpy(c.viewProj,     &m_viewProj,     sizeof(c.viewProj));
        std::memcpy(c.invViewProj,  &m_invViewProj,  sizeof(c.invViewProj));
        std::memcpy(c.prevViewProj, &m_prevViewProj, sizeof(c.prevViewProj));
        c.cameraPos[0] = m_cameraPos.x; c.cameraPos[1] = m_cameraPos.y; c.cameraPos[2] = m_cameraPos.z;
        c.nearPlane    = m_nearPlane;
        c.farPlane     = m_farPlane;
        c.froxelNear   = m_froxelNear;
        c.froxelFar    = m_froxelFar;
        c.temporalAlpha= effectiveAlpha;
        c.frameIndex   = m_frameIndex;
        c.froxelW      = kFroxelW;
        c.froxelH      = kFroxelH;
        c.froxelD      = kFroxelD;
        c.fogDensity        = m_fogDensity;
        c.fogScattering     = m_fogScattering;
        c.fogAbsorption     = m_fogAbsorption;
        c.anisotropy        = m_anisotropy;
        c.heightFogStart    = m_heightStart;
        c.heightFogFalloff  = m_heightFalloff;
        c.ambientContribution = m_ambientContribution;
        c.sunDir[0] = m_sunDir.x;   c.sunDir[1] = m_sunDir.y;   c.sunDir[2] = m_sunDir.z;
        c.sunStrength = m_sunStrength;
        c.sunColor[0] = m_sunColor.x; c.sunColor[1] = m_sunColor.y; c.sunColor[2] = m_sunColor.z;
        c.ambientColor[0] = m_ambientColor.x;
        c.ambientColor[1] = m_ambientColor.y;
        c.ambientColor[2] = m_ambientColor.z;

        // CSM (packed into the same CB).
        std::memcpy(c.shadowMatrix0, &m_shadowMatrix[0], sizeof(c.shadowMatrix0));
        std::memcpy(c.shadowMatrix1, &m_shadowMatrix[1], sizeof(c.shadowMatrix1));
        std::memcpy(c.shadowMatrix2, &m_shadowMatrix[2], sizeof(c.shadowMatrix2));
        c.cascadeSplits[0] = m_cascadeSplits.x;
        c.cascadeSplits[1] = m_cascadeSplits.y;
        c.cascadeSplits[2] = m_cascadeSplits.z;
        c.cascadeSplits[3] = 0.0f;
        c.shadowParams[0]  = m_shadowTexelSize;
        // .y is repurposed as the spot/point light count for the inject loop
        // (fits as a float since only used as integer cast in shader).
        c.shadowParams[1]  = static_cast<float>(m_spotLightCount);
        c.shadowParams[2]  = m_shadowBias;
        c.shadowParams[3]  = m_shadowStrength;
        // Camera forward — the third row of viewProj's rotation block isn't
        // exactly the world-space camera forward axis after composition with
        // projection. Renderer pushes the unit forward via a setter; if not
        // provided, fall back to (0, 0, 1) (world Z).
        c.cameraForward[0] = m_cameraForward.x;
        c.cameraForward[1] = m_cameraForward.y;
        c.cameraForward[2] = m_cameraForward.z;

        // Voxel occupancy grid. If no grid has been bound this frame (e.g.
        // SceneVoxelPass disabled), extent == 0 and the shader treats every
        // voxel-raymarch sample as out-of-bounds → falls back to "visible".
        c.voxelGridMin[0]    = m_voxelGridMin.x;
        c.voxelGridMin[1]    = m_voxelGridMin.y;
        c.voxelGridMin[2]    = m_voxelGridMin.z;
        c.voxelGridExtent[0] = m_voxelGridMax.x - m_voxelGridMin.x;
        c.voxelGridExtent[1] = m_voxelGridMax.y - m_voxelGridMin.y;
        c.voxelGridExtent[2] = m_voxelGridMax.z - m_voxelGridMin.z;
        c.voxelGridDim       = m_voxelGridDim;

        *paramCBSlot = c;
    }

    auto pBegin = [&](const char* name) -> uint32_t {
        return gfx.BeginGPUTimestamp(cl, name);
    };
    auto pEnd = [&](uint32_t r) {
        gfx.EndGPUTimestamp(cl, r);
    };

    {
        uint32_t r = pBegin("VolFog.Density"); DispatchDensity(cl);     pEnd(r);
    }
    {
        uint32_t r = pBegin("VolFog.LightInject"); DispatchLightInject(cl); pEnd(r);
    }
    {
        uint32_t r = pBegin("VolFog.Scatter"); DispatchScatter(cl);     pEnd(r);
    }

    // ---- Temporal reprojection (optional) ----------------------------------
    // Blends the just-scattered grid against the previous frame's history
    // with a neighbourhood clamp. When enabled, Apply samples the history
    // texture; otherwise it samples scattering directly.
    bool useHistoryForApply = false;
    if (m_temporalEnabled)
    {
        uint32_t r = pBegin("VolFog.Temporal");
        DispatchTemporal(cl);
        pEnd(r);
        useHistoryForApply = m_historyValid && m_externalHistoryValid;
    }
    else
    {
        // No temporal pass — push scattering into pixel-SR for the apply PS.
        TransitionTex(gfx, m_scatteringTex, m_scatteringState,
                      RHI::ResourceState::SHADER_RESOURCE, cl);
    }

    // Choose which texture the apply PS samples this frame.
    const uint64_t applySrv = useHistoryForApply
        ? gfx.GetTextureSRVGpuHandle(m_historyTex[m_historyWriteIdx])
        : gfx.GetTextureSRVGpuHandle(m_scatteringTex);

    // ---- Per-pixel raymarch tier (sun + shadow-casting spots) -------------
    // Runs AFTER the froxel density is ready (we sample it as σ_t) and
    // BEFORE the froxel apply so the froxel medium transmittance downstream
    // extinguishes the raymarched shafts the same way it attenuates the
    // rest of the lit scene.
    if (m_rmEnabled && m_rmPSO.IsValid() && m_rmCurrentTex.IsValid())
    {
        uint32_t r = pBegin("VolFog.Raymarch");
        DispatchRaymarch(cl);
        pEnd(r);

        if (m_temporalEnabled && m_rmTemporalPSO.IsValid() && m_rmHistoryTex[0].IsValid())
        {
            uint32_t rt = pBegin("VolFog.Raymarch.Temporal");
            DispatchRaymarchTemporal(cl);
            pEnd(rt);
        }
        else
        {
            // No temporal — promote current to pixel-SR so the apply PS can sample it.
            TransitionTex(gfx, m_rmCurrentTex, m_rmCurrentState,
                          RHI::ResourceState::SHADER_RESOURCE, cl);
        }
    }

    // ---- Apply (graphics) — composite onto HDR -----------------------------
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl);

    // Raymarch composite first: additive, so the sun/spot shaft contribution
    // stacks onto the HDR lighting before the froxel apply multiplies the
    // whole thing by the medium transmittance.
    if (m_rmEnabled && m_rmCurrentTex.IsValid())
    {
        uint32_t r = pBegin("VolFog.Raymarch.Apply");
        DrawRaymarchApply(cl);
        pEnd(r);
    }

    {
        uint32_t r = pBegin("VolFog.Apply");
        // Stash the SRV handle on the pass for DrawApply — keeps that helper
        // signature unchanged.
        m_applyOverrideSrv = applySrv;
        DrawApply(cl);
        m_applyOverrideSrv = 0;
        pEnd(r);
    }

    // Swap history ping-pong for next frame.
    if (m_temporalEnabled)
    {
        m_historyReadIdx  = m_historyWriteIdx;
        m_historyWriteIdx = 1u - m_historyWriteIdx;
        // Same swap for the raymarch half-res ping-pong so we always write
        // into the slot we just read from.
        m_rmHistoryReadIdx  = m_rmHistoryWriteIdx;
        m_rmHistoryWriteIdx = 1u - m_rmHistoryWriteIdx;
    }

    // Snapshot THIS frame's viewProj so next frame's temporal pass has a
    // correct prev-frame matrix to reproject against. (Renderer passing
    // m_prevViewProjNoJitter doesn't work reliably because that member is
    // only updated when TAA is active.)
    m_prevViewProj  = m_viewProj;
    m_prevCameraPos = m_cameraPos;

    return cl;
}
