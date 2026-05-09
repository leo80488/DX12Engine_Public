#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>

struct alignas(16) SkyCBData
{
    float    sunDir[3];    float sunDiskSize;       // cos(half-angle of sun core)
    float    sunColor[3];  float sunDiskIntensity;
    float    moonDir[3];   float moonDiskCos;       // cos(half-angle of moon disk)
    float    moonColor[3]; float moonVisible;       // 1 = draw moon, 0 = skip
    float    starIntensity;                          // [0,1] day/night blend
    float    starTime;                               // seconds, twinkle phase
    float    starDensity;                            // grid resolution
    float    starBrightness;                         // overall multiplier
};

// Root parameter slot for cube vertex ByteAddressBuffer (t2 space0, slot 0 of BindResource).
static constexpr uint32_t kCubeVBSlot     =  0;   // maps to root param 10 → t2
// Root parameter slot for environment cubemap (t6 space0).
static constexpr uint32_t kEnvMapRootSlot = 19;   // must match kIBLSRVSlotBase in GraphicsDX12.cpp
// Reuse the gEmissive slot (t17 space0, root 29) for the moon Texture2D when
// the moon path is active. Skybox runs after Lighting so re-binding is safe;
// next-frame lighting will rebind gEmissive.
static constexpr uint32_t kMoonTexRootSlot = 29;
// Same trick for the stars cubemap — root 30 (t18 space0) is the SSAO slot in
// Lighting.ps; Skybox doesn't read SSAO so we re-bind it to the stars cube
// during the skybox draw. Lighting next frame rebinds it back.
static constexpr uint32_t kStarsTexRootSlot = 30;

// Compute root sig slots used by the bake pass (space2).
static constexpr uint32_t kComputeCBSlot = 0;
static constexpr uint32_t kComputeUAV0   = 4;

struct alignas(16) StarsBakeCBData
{
    uint32_t cubeSize;
    float    starDensity;
    float    starBrightnessBake;
    uint32_t _pad0;
};

// Pre-expanded unit cube: 36 float3 positions (-1..+1), 6 faces × 2 triangles × 3 verts.
// The vertex IS the cubemap sampling direction (w=0 trick strips camera translation).
static const float kCubePositions[36][3] =
{
    // +X face
    { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1},
    { 1,-1,-1}, { 1, 1, 1}, { 1,-1, 1},
    // -X face
    {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1},
    {-1,-1, 1}, {-1, 1,-1}, {-1,-1,-1},
    // +Y face
    {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1},
    {-1, 1, 1}, { 1, 1,-1}, {-1, 1,-1},
    // -Y face
    {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1},
    {-1,-1,-1}, { 1,-1, 1}, {-1,-1, 1},
    // +Z face
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1},
    {-1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    // -Z face
    { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1},
    { 1,-1,-1}, {-1, 1,-1}, { 1, 1,-1},
};

SkyboxPass::SkyboxPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

void SkyboxPass::Setup(RG::RenderGraphBuilder& b)
{
    // Declare depth as write so the RenderGraph emits the
    // SHADER_RESOURCE → DEPTHSTENCIL barrier before this pass.
    b.WriteDepthStencil(m_depth);
    // BuiltinTexture::None — we manually bind the HDR RTV + depth inside Execute.
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void SkyboxPass::Init(IGraphicsDevice& gfx)
{
    // Trilinear-wrap sampler for cubemap filtering.
    RHI::SamplerDesc sd;
    sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
    sd.address_u = RHI::TextureAddressMode::WRAP;
    sd.address_v = RHI::TextureAddressMode::WRAP;
    sd.address_w = RHI::TextureAddressMode::WRAP;
    gfx.CreateSampler(sd, m_sampler);

    // Upload the cube vertex buffer (ByteAddressBuffer, 36 × float3).
    {
        RHI::GPUBufferDesc d;
        d.size       = sizeof(kCubePositions);
        d.usage      = RHI::Usage::DEFAULT;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(d, m_cubeVB, kCubePositions))
            LOG_ERROR("SkyboxPass: failed to create cube vertex buffer");
    }

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Skybox_VS,    RHI::ShaderStage::VS, "Skybox.vs.hlsl");
    m_shaderLib.Register(ShaderID::Skybox_PS,    RHI::ShaderStage::PS, "Skybox.ps.hlsl");
    m_shaderLib.Register(ShaderID::StarsBake_CS, RHI::ShaderStage::CS,
                         "StarsBake.cs.hlsl",   "CSMain");
    m_psoCache.Init(gfx, m_shaderLib);

    // Star bake compute PSO + cubemap target (R11G11B10F, 6-face).
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::StarsBake_CS);
        if (cs)
        {
            RHI::PipelineStateDesc d{};
            d.cs = cs;
            if (!gfx.CreatePipelineState(d, m_starsBakePSO))
                LOG_ERROR("SkyboxPass: stars bake PSO failed");
        }

        RHI::TextureDesc td{};
        td.width      = kStarsCubeSize;
        td.height     = kStarsCubeSize;
        td.array_size = 6;
        td.mip_levels = 1;
        // RGBA16F — alpha channel stores the per-star twinkle phase (0 = no
        // star, >0 = phase normalized to [0, 1]). Runtime shader samples
        // rgba and uses a to modulate star brightness in sync per star.
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.misc_flags = RHI::ResourceMiscFlag::TEXTURECUBE;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!gfx.CreateTexture(td, m_starsCubemap))
            LOG_ERROR("SkyboxPass: stars cubemap create failed");

        RHI::GPUBufferDesc bd{};
        bd.size       = (sizeof(StarsBakeCBData) + 255) & ~255u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_starsBakeCB))
            m_starsBakeCBMapped = gfx.MapBuffer(m_starsBakeCB);

        // Cube sampler — wrap is fine since the cubemap face math handles
        // wrap-around. Reuse a linear-wrap sampler shared with the env cube.
        RHI::SamplerDesc cs2;
        cs2.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        cs2.address_u = RHI::TextureAddressMode::WRAP;
        cs2.address_v = RHI::TextureAddressMode::WRAP;
        cs2.address_w = RHI::TextureAddressMode::WRAP;
        gfx.CreateSampler(cs2, m_starsCubeSampler);
    }

    // SkyCB — persistent UPLOAD CB for sun disk parameters (bound at b2).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = (sizeof(SkyCBData) + 255) & ~255u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_skyCB))
            m_skyCBMapped = gfx.MapBuffer(m_skyCB);
        else
            LOG_ERROR("SkyboxPass: SkyCB creation failed");
    }

    if (!m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_ERROR("SkyboxPass: PSO creation failed");
    else
        LOG_INFO("SkyboxPass: PSO ready");
}

PSODesc SkyboxPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID       = ShaderID::Skybox_VS;
    desc.psID       = ShaderID::Skybox_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = true;

    // Reversed-Z: skybox VS now outputs NDC z=0 (far plane). Depth compare is
    // GREATER_EQUAL so 0 >= 0 passes only where no opaque geometry has written
    // a closer (>0) depth yet.
    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;

    desc.bs.render_target[0].render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;
    return desc;
}

RHI::CommandList SkyboxPass::Execute(RHI::CommandList cl)
{
    if (!m_envMapGpuHandle) return cl; // no cubemap assigned yet
    if (!m_cubeVB.IsValid()) return cl;

    auto& gfx = cl.GetDevice();

    // ---- One-time star cubemap bake ----------------------------------------
    // First Execute() call dispatches the procedural star generator into the
    // 6-face cubemap, then transitions it to SHADER_RESOURCE. Subsequent
    // frames just sample.
    if (!m_starsBaked && m_starsBakePSO.IsValid() && m_starsCubemap.IsValid())
    {
        if (m_starsBakeCBMapped)
        {
            StarsBakeCBData c{};
            c.cubeSize           = kStarsCubeSize;
            c.starDensity        = m_starDensity;
            c.starBrightnessBake = m_starBrightness;
            std::memcpy(m_starsBakeCBMapped, &c, sizeof(c));
        }

        gfx.BindComputePipelineState(m_starsBakePSO, cl);
        gfx.SetComputeRootCBV(kComputeCBSlot, m_starsBakeCB, cl);
        gfx.SetComputeDescriptorTable(kComputeUAV0,
            gfx.GetTextureUAVGpuHandle(m_starsCubemap), cl);

        // Dispatch: one thread per (texel, face). 8×8 thread group, z = 6.
        gfx.DispatchCompute((kStarsCubeSize + 7) / 8,
                            (kStarsCubeSize + 7) / 8, 6, cl);

        // UAV → SRV for skybox sampling.
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_starsCubemap,
            RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE), cl);
        m_starsCubemapState = RHI::ResourceState::SHADER_RESOURCE;
        m_starsBaked = true;
        LOG_INFO("SkyboxPass: stars cubemap baked (%ux%u × 6 faces)",
                 kStarsCubeSize, kStarsCubeSize);
    }

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc());
    if (!pso || !pso->IsValid()) return cl;

    // Bind HDR RTV + depth (no clear). The depth is already in DEPTHSTENCIL state
    // from the barrier emitted by the RenderGraph (WriteDepthStencil declared in Setup).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl);

    // Upload the latest sun / moon parameters into SkyCB (b2).
    if (m_skyCBMapped)
    {
        SkyCBData d{};
        d.sunDir[0] = m_sunDir.x;     d.sunDir[1] = m_sunDir.y;     d.sunDir[2] = m_sunDir.z;
        d.sunDiskSize = m_sunDiskCos;
        d.sunColor[0] = m_sunColor.x; d.sunColor[1] = m_sunColor.y; d.sunColor[2] = m_sunColor.z;
        d.sunDiskIntensity = m_sunDiskIntensity;
        d.moonDir[0]    = m_moonDir.x;   d.moonDir[1] = m_moonDir.y;   d.moonDir[2] = m_moonDir.z;
        d.moonDiskCos   = m_moonDiskCos;
        d.moonColor[0]  = m_moonColor.x; d.moonColor[1] = m_moonColor.y; d.moonColor[2] = m_moonColor.z;
        d.moonVisible   = (m_moonVisible && m_moonTexHandle) ? 1.0f : 0.0f;
        d.starIntensity = m_starIntensity;
        d.starTime      = m_starTime;
        d.starDensity   = m_starDensity;
        d.starBrightness= m_starBrightness;
        std::memcpy(m_skyCBMapped, &d, sizeof(d));
    }

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);
    cl.BindCBByName(0, "PerView");           // b1: viewProj for the cube transform
    cl.BindCBByName(1, "SkyCB");             // b2: sun params for the analytic disk
    cl.BindBufferSRV(kCubeVBSlot, m_cubeVB); // t2: cube ByteAddressBuffer
    cl.BindDescriptorTableHandle(kEnvMapRootSlot, m_envMapGpuHandle); // t6: TextureCube
    // Moon texture (t17 space0 — gEmissive's slot, repurposed for skybox draw
    // because Lighting always re-binds gEmissive next frame). Bind something
    // valid even when sun is active so the root sig stays satisfied; the
    // shader gates sampling on isMoon flag.
    if (m_moonTexHandle)
        cl.BindDescriptorTableHandle(kMoonTexRootSlot, m_moonTexHandle);
    // Stars cubemap (t18 space0 = SSAO slot in lighting; rebound here for the
    // skybox draw, lighting next frame rebinds SSAO).
    if (m_starsBaked && m_starsCubemap.IsValid())
    {
        cl.BindDescriptorTableHandle(kStarsTexRootSlot,
            cl.GetDevice().GetTextureSRVGpuHandle(m_starsCubemap));
    }
    if (m_sampler >= 0)
        cl.BindSampler(0, m_sampler);

    cl.DrawInstanced(36, 1, 0, 0);
    return cl;
}
