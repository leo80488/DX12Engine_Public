#include "RenderGraph/RenderPass/CloudPass.h"
#include "ECS/CloudComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

using namespace DirectX;

// Compute root sig slots (shared with all compute passes — see GraphicsDX12).
static constexpr uint32_t kCBSlot   = 0;   // b0 space2
static constexpr uint32_t kSRVT0    = 1;   // t0 space2 — base shape noise
static constexpr uint32_t kSRVT1    = 2;   // t1 space2 — detail erosion noise
static constexpr uint32_t kSRVT2    = 3;   // t2 space2 — weather map
static constexpr uint32_t kSRVT3    = 7;   // t3 space2 — scene depth (matches VolumetricFogPass)
static constexpr uint32_t kUAV0     = 4;   // u0 space2
static constexpr uint32_t kUAV1     = 5;   // u1 space2 — per-texel march distance

// Graphics root sig slot for the apply PS — same pattern as VolumetricFogPass
// re-purposes for its half-res raymarch result.
static constexpr uint32_t kEnvMapRootSlot    = 19; // t6 space0
static constexpr uint32_t kCloudDistRootSlot = 20; // t7 space0 (second IBL slot)

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
        if (m_baseNoiseTex.IsValid())   m_gfx->DestroyTexture(m_baseNoiseTex);
        if (m_detailNoiseTex.IsValid()) m_gfx->DestroyTexture(m_detailNoiseTex);
        if (m_weatherTex.IsValid())     m_gfx->DestroyTexture(m_weatherTex);
        if (m_cloudTex.IsValid())       m_gfx->DestroyTexture(m_cloudTex);
        if (m_cloudDistTex.IsValid())   m_gfx->DestroyTexture(m_cloudDistTex);
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
    m_shaderLib.Register(ShaderID::CloudNoiseBake_CS,       RHI::ShaderStage::CS,
                         "CloudNoiseBake.cs.hlsl",       "main");
    m_shaderLib.Register(ShaderID::CloudDetailNoiseBake_CS, RHI::ShaderStage::CS,
                         "CloudDetailNoiseBake.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::CloudWeatherBake_CS,     RHI::ShaderStage::CS,
                         "CloudWeatherBake.cs.hlsl",     "main");
    m_shaderLib.Register(ShaderID::CloudRaymarch_CS,        RHI::ShaderStage::CS,
                         "CloudRaymarch.cs.hlsl",        "main");
    m_shaderLib.Register(ShaderID::CloudComposite_VS,       RHI::ShaderStage::VS,
                         "CloudComposite.vs.hlsl",       "main");
    m_shaderLib.Register(ShaderID::CloudComposite_PS,       RHI::ShaderStage::PS,
                         "CloudComposite.ps.hlsl",       "main");

    auto makeCs = [&](ShaderID id, RHI::PipelineState& out, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("CloudPass: %s_CS not found", tag); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, out))
            LOG_ERROR("CloudPass: %s PSO failed", tag);
    };
    makeCs(ShaderID::CloudNoiseBake_CS,       m_noiseBakePSO,   "NoiseBake");
    makeCs(ShaderID::CloudDetailNoiseBake_CS, m_detailBakePSO,  "DetailBake");
    makeCs(ShaderID::CloudWeatherBake_CS,     m_weatherBakePSO, "WeatherBake");
    makeCs(ShaderID::CloudRaymarch_CS,        m_raymarchPSO,    "Raymarch");

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
        desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
        if (!m_psoCache.GetOrCreate(desc))
            LOG_ERROR("CloudPass: composite PSO failed");
    }

    // ---- Baked lookup textures (UAV/SRV, written once on first Execute) ----
    auto make3D = [&](uint32_t dim, RHI::Texture& out, const char* tag) {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
        td.width      = dim;
        td.height     = dim;
        td.depth      = dim;
        td.mip_levels = 1;
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!gfx.CreateTexture(td, out))
            LOG_ERROR("CloudPass: %s 3D texture create failed", tag);
    };
    make3D(kBaseNoiseDim,   m_baseNoiseTex,   "base noise");
    make3D(kDetailNoiseDim, m_detailNoiseTex, "detail noise");

    {
        RHI::TextureDesc td{};
        td.width      = kWeatherDim;
        td.height     = kWeatherDim;
        td.depth      = 1;
        td.mip_levels = 1;
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!gfx.CreateTexture(td, m_weatherTex))
            LOG_ERROR("CloudPass: weather texture create failed");
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

    LOG_SUCCESS("CloudPass: initialised (base %u^3 + detail %u^3 + weather %u^2, "
                "raymarch quarter-res)",
                kBaseNoiseDim, kDetailNoiseDim, kWeatherDim);
}

// ---------------------------------------------------------------------------
void CloudPass::RebuildCloudTexture(uint32_t fullW, uint32_t fullH)
{
    if (!m_gfx) return;
    if (m_cloudTex.IsValid())     m_gfx->DestroyTexture(m_cloudTex);
    if (m_cloudDistTex.IsValid()) m_gfx->DestroyTexture(m_cloudDistTex);

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
        LOG_ERROR("CloudPass: cloud quarter-res texture create failed");

    td.format = RHI::Format::R32_FLOAT;
    if (!m_gfx->CreateTexture(td, m_cloudDistTex))
        LOG_ERROR("CloudPass: cloud march-distance texture create failed");

    m_cloudState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_cloudDistState = RHI::ResourceState::UNORDERED_ACCESS;
    m_lastFullW      = fullW;
    m_lastFullH      = fullH;
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
    m_paramsEnabled    = c.enabled;
    m_bottomAlt        = c.bottomAltitude;
    m_topAlt           = c.topAltitude;
    m_coverage         = c.coverage;
    m_density          = c.density;
    m_baseNoiseScale   = c.noiseScale;
    m_detailNoiseScale = c.detailNoiseScale;
    m_detailStrength   = c.detailStrength;
    m_weatherScale     = c.weatherScale;
    m_cloudTypeBias    = c.cloudTypeBias;
    m_anvilBias        = c.anvilBias;
    m_windDir          = c.windDirection;
    m_windSpeed        = c.windSpeed;
    m_phaseFwdG        = c.anisotropy;
    m_phaseBackG       = c.phaseBackG;
    m_phaseBlend       = c.phaseBlend;
    m_silverIntensity  = c.silverIntensity;
    m_silverSpread     = c.silverSpread;
    m_extinction       = c.extinction;
    m_ambient          = c.ambientStrength;
    m_ambientTint      = c.ambientTint;
    m_cloudColor       = c.cloudColor;
    m_maxSteps         = c.maxSteps;

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
    c.cameraPos[0]     = m_cameraPos.x;
    c.cameraPos[1]     = m_cameraPos.y;
    c.cameraPos[2]     = m_cameraPos.z;
    c.nearZ            = m_nearZ;
    c.farZ             = m_farZ;
    c.bottomAltitude   = m_bottomAlt;
    c.topAltitude      = std::max(m_topAlt, m_bottomAlt + 1.0f);
    c.coverage         = m_coverage;
    c.density          = m_density;
    c.baseNoiseScale   = m_baseNoiseScale;
    c.detailNoiseScale = m_detailNoiseScale;
    c.detailStrength   = m_detailStrength;
    c.weatherScale     = m_weatherScale;
    c.cloudTypeBias    = m_cloudTypeBias;
    c.anvilBias        = m_anvilBias;
    c.extinction       = m_extinction;

    c.sunDir[0]        = m_sunDir.x;
    c.sunDir[1]        = m_sunDir.y;
    c.sunDir[2]        = m_sunDir.z;
    c.ambientStrength  = m_ambient;
    c.sunColor[0]      = m_sunColor.x;
    c.sunColor[1]      = m_sunColor.y;
    c.sunColor[2]      = m_sunColor.z;
    c.phaseFwdG        = m_phaseFwdG;
    c.ambientTint[0]   = m_ambientTint.x;
    c.ambientTint[1]   = m_ambientTint.y;
    c.ambientTint[2]   = m_ambientTint.z;
    c.phaseBackG       = m_phaseBackG;
    c.cloudColor[0]    = m_cloudColor.x;
    c.cloudColor[1]    = m_cloudColor.y;
    c.cloudColor[2]    = m_cloudColor.z;
    c.phaseBlend       = m_phaseBlend;

    // Normalize wind direction and scale by accumulated offset distance.
    XMFLOAT3 w = m_windDir;
    float wlen = std::sqrt(w.x*w.x + w.y*w.y + w.z*w.z);
    if (wlen > 1e-6f) { w.x /= wlen; w.y /= wlen; w.z /= wlen; }
    c.windOffset[0]    = w.x * m_windOffset;
    c.windOffset[1]    = w.y * m_windOffset;
    c.windOffset[2]    = w.z * m_windOffset;
    c.silverIntensity  = m_silverIntensity;

    c.silverSpread     = m_silverSpread;
    c.frameIndex       = static_cast<float>(m_frameIndex % 8u);
    c.maxSteps         = std::clamp(m_maxSteps, 24.f, 256.f);
    c.maxTraceDist     = kMaxTraceDist;

    c.halfResW = static_cast<float>(m_cloudW);
    c.halfResH = static_cast<float>(m_cloudH);
    c.fullResW = static_cast<float>(m_lastFullW);
    c.fullResH = static_cast<float>(m_lastFullH);

    *slot = c;
}

// ---------------------------------------------------------------------------
void CloudPass::BakeNoiseOnce(RHI::CommandList cl)
{
    if (m_noiseBaked) return;
    if (!m_noiseBakePSO.IsValid() || !m_detailBakePSO.IsValid() ||
        !m_weatherBakePSO.IsValid())
        return;
    if (!m_baseNoiseTex.IsValid() || !m_detailNoiseTex.IsValid() ||
        !m_weatherTex.IsValid())
        return;
    IGraphicsDevice& gfx = *m_gfx;

    auto bake = [&](const RHI::PipelineState& pso, const RHI::Texture& tex,
                    RHI::ResourceState& state,
                    uint32_t gx, uint32_t gy, uint32_t gz)
    {
        TransitionTex(gfx, tex, state, RHI::ResourceState::UNORDERED_ACCESS, cl);
        gfx.BindComputePipelineState(pso, cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(tex), cl);
        gfx.DispatchCompute(gx, gy, gz, cl);
        TransitionTex(gfx, tex, state, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    };

    bake(m_noiseBakePSO,   m_baseNoiseTex,   m_baseNoiseState,
         kBaseNoiseDim / 8, kBaseNoiseDim / 8, kBaseNoiseDim / 8);     // 8x8x8 groups
    bake(m_detailBakePSO,  m_detailNoiseTex, m_detailNoiseState,
         kDetailNoiseDim / 4, kDetailNoiseDim / 4, kDetailNoiseDim / 4); // 4x4x4 groups
    bake(m_weatherBakePSO, m_weatherTex,     m_weatherState,
         kWeatherDim / 8, kWeatherDim / 8, 1);                          // 8x8 groups

    m_noiseBaked = true;
    LOG_INFO("CloudPass: baked base %u^3 + detail %u^3 + weather %u^2",
             kBaseNoiseDim, kDetailNoiseDim, kWeatherDim);
}

// ---------------------------------------------------------------------------
void CloudPass::DispatchRaymarch(RHI::CommandList cl)
{
    if (!m_raymarchPSO.IsValid() || !m_cloudTex.IsValid()
        || !m_cloudDistTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_cloudTex, m_cloudState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_cloudDistTex, m_cloudDistState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_baseNoiseTex, m_baseNoiseState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_detailNoiseTex, m_detailNoiseState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_weatherTex, m_weatherState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);

    gfx.BindComputePipelineState(m_raymarchPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRVT0,
        gfx.GetTextureSRVGpuHandle(m_baseNoiseTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT1,
        gfx.GetTextureSRVGpuHandle(m_detailNoiseTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT2,
        gfx.GetTextureSRVGpuHandle(m_weatherTex), cl);
    // t3 space2 = scene depth (sampled to clamp ray exit past opaque geometry)
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(kSRVT3,
            gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    // u0 space2 = quarter-res output; u1 space2 = per-texel march distance
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(m_cloudTex), cl);
    gfx.SetComputeDescriptorTable(kUAV1,
        gfx.GetTextureUAVGpuHandle(m_cloudDistTex), cl);

    gfx.DispatchCompute((m_cloudW + 7) / 8, (m_cloudH + 7) / 8, 1, cl);

    TransitionTex(gfx, m_cloudTex, m_cloudState,
                  RHI::ResourceState::SHADER_RESOURCE, cl);
    TransitionTex(gfx, m_cloudDistTex, m_cloudDistState,
                  RHI::ResourceState::SHADER_RESOURCE, cl);

    // Only now is the texture safe to serve to other passes (lens flare).
    m_renderedThisFrame = true;
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
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

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

    // CB (b3 space0, registered by Renderer as "CloudApplyCB") + full-res
    // depth at t5 — both needed by the depth-aware upsample / shell guard.
    // Same pattern as VolumetricFogPass::DrawApply.
    cl.BindCBByName(2, "CloudApplyCB");
    cl.BindSRVByHandle(3, m_depth);

    // Cloud quarter-res RGBA16F bound at t6 space0 — same slot VolumetricFog
    // re-purposes for its half-res raymarch source. March distance at t7.
    cl.BindDescriptorTableHandle(kEnvMapRootSlot,
        m_gfx->GetTextureSRVGpuHandle(m_cloudTex));
    cl.BindDescriptorTableHandle(kCloudDistRootSlot,
        m_gfx->GetTextureSRVGpuHandle(m_cloudDistTex));
    if (m_linearSamplerSlot >= 0)
        cl.BindSampler(0, m_linearSamplerSlot);

    cl.DrawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
uint64_t CloudPass::GetCloudSrvHandle() const
{
    if (!m_renderedThisFrame || !m_gfx || !m_cloudTex.IsValid()) return 0;
    if (m_cloudState != RHI::ResourceState::SHADER_RESOURCE) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_cloudTex);
}

// ---------------------------------------------------------------------------
RHI::CommandList CloudPass::Execute(RHI::CommandList cl)
{
    // Reset BEFORE the early-returns: a disabled / wireframe / no-component
    // frame must not serve last frame's transmittance to the lens flare.
    m_renderedThisFrame = false;

    if (!m_enabled || !m_paramsEnabled || m_viewModeHidden) return cl;
    if (!m_gfx) return cl;
    IGraphicsDevice& gfx = *m_gfx;

    // First Execute: bake the noise volumes + weather map once.
    BakeNoiseOnce(cl);

    // Rebuild quarter-res buffer on resize.
    const uint32_t fullW = gfx.GetRenderWidth();
    const uint32_t fullH = gfx.GetRenderHeight();
    if (fullW == 0 || fullH == 0) return cl;
    if (fullW != m_lastFullW || fullH != m_lastFullH || !m_cloudTex.IsValid())
        RebuildCloudTexture(fullW, fullH);

    ++m_frameIndex;

    // Upload CB + raymarch.
    UploadCB();
    DispatchRaymarch(cl);

    // Composite into HDR with alpha-over blend.
    Composite(cl);

    return cl;
}
