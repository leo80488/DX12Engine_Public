#include "RenderGraph/RenderPass/CloudPass.h"
#include "ECS/CloudComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

using namespace DirectX;

// Compute root sig slots (shared with all compute passes — see GraphicsDX12).
static constexpr uint32_t kCBSlot   = 0;   // b0 space2
static constexpr uint32_t kSRVT0    = 1;   // t0 space2
static constexpr uint32_t kSRVT3    = 7;   // t3 space2 — scene depth (matches VolumetricFogPass)
static constexpr uint32_t kUAV0     = 4;   // u0 space2

// Graphics root sig slot for the apply PS — same pattern as VolumetricFogPass
// re-purposes for its half-res raymarch result.
static constexpr uint32_t kEnvMapRootSlot = 19; // t6 space0

using CloudConstants = CloudPass::CloudConstants;

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

// ---------------------------------------------------------------------------
CloudPass::CloudPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

CloudPass::~CloudPass()
{
    if (m_gfx)
    {
        m_cb.Destroy(*m_gfx);
        if (m_noiseTex.IsValid()) m_gfx->DestroyTexture(m_noiseTex);
        if (m_cloudTex.IsValid()) m_gfx->DestroyTexture(m_cloudTex);
    }
}

// ---------------------------------------------------------------------------
void CloudPass::Setup(RG::RenderGraphBuilder& b)
{
    // IMPORTANT: do NOT declare HdrSceneColor as the colour target — the graph
    // would CLEAR HDR on entry, wiping Lighting+Skybox to black before our
    // composite even runs. Bind HDR manually inside Composite() instead, the
    // same pattern SkyboxPass / VolumetricFogPass use.
    b.ReadSRV(m_depth);                            // raymarch samples scene depth
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// ---------------------------------------------------------------------------
void CloudPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Shader registration -----------------------------------------------
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::CloudNoiseBake_CS, RHI::ShaderStage::CS,
                         "CloudNoiseBake.cs.hlsl",  "main");
    m_shaderLib.Register(ShaderID::CloudRaymarch_CS,  RHI::ShaderStage::CS,
                         "CloudRaymarch.cs.hlsl",   "main");
    m_shaderLib.Register(ShaderID::CloudComposite_VS, RHI::ShaderStage::VS,
                         "CloudComposite.vs.hlsl",  "main");
    m_shaderLib.Register(ShaderID::CloudComposite_PS, RHI::ShaderStage::PS,
                         "CloudComposite.ps.hlsl",  "main");

    auto makeCs = [&](ShaderID id, RHI::PipelineState& out, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("CloudPass: %s_CS not found", tag); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, out))
            LOG_ERROR("CloudPass: %s PSO failed", tag);
    };
    makeCs(ShaderID::CloudNoiseBake_CS, m_noiseBakePSO, "NoiseBake");
    makeCs(ShaderID::CloudRaymarch_CS,  m_raymarchPSO,  "Raymarch");

    // Composite graphics PSO (alpha-over: src=ONE, dst=SRC_ALPHA).
    m_psoCache.Init(gfx, m_shaderLib);
    {
        PSODesc desc;
        desc.vsID        = ShaderID::CloudComposite_VS;
        desc.psID        = ShaderID::CloudComposite_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cull_mode         = RHI::CullMode::NONE;
        desc.rs.depth_clip_enable = false;
        desc.dss.depth_enable     = false;
        desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
        auto& bs = desc.bs.render_target[0];
        bs.blend_enable           = true;
        bs.src_blend              = RHI::Blend::ONE;
        bs.dest_blend             = RHI::Blend::SRC_ALPHA;
        bs.blend_op               = RHI::BlendOp::ADD;
        bs.src_blend_alpha        = RHI::Blend::ZERO;
        bs.dest_blend_alpha       = RHI::Blend::ONE;
        bs.blend_op_alpha         = RHI::BlendOp::ADD;
        bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;
        if (!m_psoCache.GetOrCreate(desc))
            LOG_ERROR("CloudPass: composite PSO failed");
    }

    // ---- Noise volume (128^3 R8 UAV/SRV) — baked once on first Execute -----
    {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
        td.width      = kNoiseDim;
        td.height     = kNoiseDim;
        td.depth      = kNoiseDim;
        td.mip_levels = 1;
        td.format     = RHI::Format::R8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!gfx.CreateTexture(td, m_noiseTex))
            LOG_ERROR("CloudPass: noise 3D texture create failed");
    }

    // ---- Constant buffer ---------------------------------------------------
    m_cb.Create(gfx, "Cloud.CB");

    // ---- Linear sampler (composite upsample) -------------------------------
    {
        RHI::SamplerDesc sd;
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        gfx.CreateSampler(sd, m_linearSamplerSlot);
    }

    LOG_SUCCESS("CloudPass: initialised (noise %u^3, raymarch quarter-res)",
                kNoiseDim);
}

// ---------------------------------------------------------------------------
void CloudPass::RebuildCloudTexture(uint32_t fullW, uint32_t fullH)
{
    if (!m_gfx) return;
    if (m_cloudTex.IsValid()) m_gfx->DestroyTexture(m_cloudTex);

    // Quarter-res = floor(w/4) round-up.
    m_cloudW = (fullW + 3) / 4;
    m_cloudH = (fullH + 3) / 4;
    if (m_cloudW == 0 || m_cloudH == 0) return;

    RHI::TextureDesc td{};
    td.width      = m_cloudW;
    td.height     = m_cloudH;
    td.depth      = 1;
    td.mip_levels = 1;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    if (!m_gfx->CreateTexture(td, m_cloudTex))
        LOG_ERROR("CloudPass: cloud half-res texture create failed");

    m_cloudState  = RHI::ResourceState::UNORDERED_ACCESS;
    m_lastFullW   = fullW;
    m_lastFullH   = fullH;
    LOG_INFO("CloudPass: quarter-res rebuilt %ux%u", m_cloudW, m_cloudH);
}

// ---------------------------------------------------------------------------
void CloudPass::SetCamera(const XMFLOAT4X4& invViewProj,
                          const XMFLOAT3&   cameraPos,
                          float nearZ, float farZ)
{
    m_invViewProj = invViewProj;
    m_cameraPos   = cameraPos;
    m_nearZ       = nearZ;
    m_farZ        = farZ;
}

void CloudPass::SetSun(const XMFLOAT3& dir, const XMFLOAT3& color)
{
    m_sunDir   = dir;
    m_sunColor = color;
}

void CloudPass::SetParams(const CloudComponent& c, float dt)
{
    m_paramsEnabled = c.enabled;
    m_bottomAlt     = c.bottomAltitude;
    m_topAlt        = c.topAltitude;
    m_coverage      = c.coverage;
    m_density       = c.density;
    m_noiseScale    = c.noiseScale;
    m_windDir       = c.windDirection;
    m_windSpeed     = c.windSpeed;
    m_anisotropy    = c.anisotropy;
    m_extinction    = c.extinction;
    m_ambient       = c.ambientStrength;
    m_cloudColor    = c.cloudColor;

    // Accumulate wind distance in metres so the shader gets a stable offset
    // (independent of wind speed changes mid-flight).
    m_windOffset += m_windSpeed * std::max(0.f, dt);
    // Wrap to avoid float precision drift over long sessions.
    if (m_windOffset > 1.0e6f) m_windOffset -= 1.0e6f;
}

// ---------------------------------------------------------------------------
void CloudPass::UploadCB()
{
    if (!m_gfx) return;
    auto* slot = m_cb.Current(*m_gfx);
    if (!slot) return;

    CloudConstants c{};
    std::memcpy(c.invViewProj, &m_invViewProj, sizeof(c.invViewProj));
    c.cameraPos[0]    = m_cameraPos.x;
    c.cameraPos[1]    = m_cameraPos.y;
    c.cameraPos[2]    = m_cameraPos.z;
    c.nearZ           = m_nearZ;
    c.farZ            = m_farZ;
    c.bottomAltitude  = m_bottomAlt;
    c.topAltitude     = m_topAlt;
    c.coverage        = m_coverage;
    c.density         = m_density;
    c.noiseScale      = m_noiseScale;
    c.anisotropy      = m_anisotropy;
    c.extinction      = m_extinction;

    c.sunDir[0]       = m_sunDir.x;
    c.sunDir[1]       = m_sunDir.y;
    c.sunDir[2]       = m_sunDir.z;
    c.ambientStrength = m_ambient;
    c.sunColor[0]     = m_sunColor.x;
    c.sunColor[1]     = m_sunColor.y;
    c.sunColor[2]     = m_sunColor.z;
    c.cloudColor[0]   = m_cloudColor.x;
    c.cloudColor[1]   = m_cloudColor.y;
    c.cloudColor[2]   = m_cloudColor.z;

    // Normalize wind direction and scale by accumulated offset distance.
    XMFLOAT3 w = m_windDir;
    float wlen = std::sqrt(w.x*w.x + w.y*w.y + w.z*w.z);
    if (wlen > 1e-6f) { w.x /= wlen; w.y /= wlen; w.z /= wlen; }
    c.windOffset[0] = w.x * m_windOffset;
    c.windOffset[1] = w.y * m_windOffset;
    c.windOffset[2] = w.z * m_windOffset;

    c.halfResW = static_cast<float>(m_cloudW);
    c.halfResH = static_cast<float>(m_cloudH);
    c.fullResW = static_cast<float>(m_lastFullW);
    c.fullResH = static_cast<float>(m_lastFullH);

    *slot = c;
}

// ---------------------------------------------------------------------------
void CloudPass::BakeNoiseOnce(RHI::CommandList cl)
{
    if (m_noiseBaked || !m_noiseBakePSO.IsValid() || !m_noiseTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_noiseTex, m_noiseState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_noiseBakePSO, cl);
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(m_noiseTex), cl);
    // 128^3 with 8x8x8 thread groups
    gfx.DispatchCompute(kNoiseDim / 8, kNoiseDim / 8, kNoiseDim / 8, cl);

    TransitionTex(gfx, m_noiseTex, m_noiseState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    m_noiseBaked = true;
    LOG_INFO("CloudPass: 3D noise volume baked (%u^3)", kNoiseDim);
}

// ---------------------------------------------------------------------------
void CloudPass::DispatchRaymarch(RHI::CommandList cl)
{
    if (!m_raymarchPSO.IsValid() || !m_cloudTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_cloudTex, m_cloudState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_noiseTex, m_noiseState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);

    gfx.BindComputePipelineState(m_raymarchPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    // t0 space2 = noise 3D
    gfx.SetComputeDescriptorTable(kSRVT0,
        gfx.GetTextureSRVGpuHandle(m_noiseTex), cl);
    // t3 space2 = scene depth (sampled to clamp ray exit past opaque geometry)
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(kSRVT3,
            gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    // u0 space2 = quarter-res output
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(m_cloudTex), cl);

    gfx.DispatchCompute((m_cloudW + 7) / 8, (m_cloudH + 7) / 8, 1, cl);

    TransitionTex(gfx, m_cloudTex, m_cloudState,
                  RHI::ResourceState::SHADER_RESOURCE, cl);
}

// ---------------------------------------------------------------------------
void CloudPass::Composite(RHI::CommandList cl)
{
    PSODesc desc;
    desc.vsID        = ShaderID::CloudComposite_VS;
    desc.psID        = ShaderID::CloudComposite_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = false;
    desc.dss.depth_enable     = false;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    auto& bs = desc.bs.render_target[0];
    bs.blend_enable           = true;
    bs.src_blend              = RHI::Blend::ONE;
    bs.dest_blend             = RHI::Blend::SRC_ALPHA;
    bs.blend_op               = RHI::BlendOp::ADD;
    bs.src_blend_alpha        = RHI::Blend::ZERO;
    bs.dest_blend_alpha       = RHI::Blend::ONE;
    bs.blend_op_alpha         = RHI::BlendOp::ADD;
    bs.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return;

    // Bind HDR colour + depth manually (Setup declared None). Depth comes
    // from the graph-managed handle; we just sample it via the SRV bound to
    // the raymarch CS above — for the composite it's bound as the DSV but
    // depth test is disabled in the PSO, so this is just for the format match.
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl);

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    // Cloud half-res RGBA16F bound at t6 space0 — same slot VolumetricFog
    // re-purposes for its half-res raymarch source.
    cl.BindDescriptorTableHandle(kEnvMapRootSlot,
        m_gfx->GetTextureSRVGpuHandle(m_cloudTex));
    if (m_linearSamplerSlot >= 0)
        cl.BindSampler(0, m_linearSamplerSlot);

    cl.DrawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
RHI::CommandList CloudPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled || !m_paramsEnabled || m_viewModeHidden) return cl;
    if (!m_gfx) return cl;
    IGraphicsDevice& gfx = *m_gfx;

    // First Execute: bake the 3D noise volume once.
    BakeNoiseOnce(cl);

    // Rebuild quarter-res buffer on resize.
    const uint32_t fullW = gfx.GetRenderWidth();
    const uint32_t fullH = gfx.GetRenderHeight();
    if (fullW == 0 || fullH == 0) return cl;
    if (fullW != m_lastFullW || fullH != m_lastFullH || !m_cloudTex.IsValid())
        RebuildCloudTexture(fullW, fullH);

    // Upload CB + raymarch.
    UploadCB();
    DispatchRaymarch(cl);

    // Composite into HDR with alpha-over blend. The render graph already set
    // the HDR colour as the colour target (declared in Setup).
    Composite(cl);

    return cl;
}
