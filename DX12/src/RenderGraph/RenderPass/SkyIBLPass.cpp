#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

// Compute root sig slots (shared with Bloom/AutoExposure — see
// GraphicsDX12::CreateComputeRootSignature).
static constexpr uint32_t kCBSlot = 0; // b0 space2
static constexpr uint32_t kSRV0   = 1; // t0 space2
static constexpr uint32_t kUAV0   = 4; // u0 space2

static constexpr uint32_t kSHCoeffCount  = 9;
static constexpr uint32_t kSHSampleCount = 2048;

// Per-mip GGX importance sample count. 1024 was overkill for a smooth
// atmospheric sky — Karis's split-sum paper shows 32-256 samples is usually
// indistinguishable from 1024 once the source cubemap is smooth. The table
// below trades near-zero visual difference for ≈ 8x lower prefilter cost:
//   mip 0 (roughness ≈ 0) — 1 sample, we just copy the source direction
//   mip 1–2 (sharp, high-detail reflections) — 128
//   mip 3   (medium)      — 64
//   mip 4–6 (rough/blurry, lobe very wide but target resolution tiny) — 32
static constexpr uint32_t kPrefilterSampleTable[7] = {
    1,      // mip 0 (128^2)
    128,    // mip 1 ( 64^2)
    128,    // mip 2 ( 32^2)
    64,     // mip 3 ( 16^2)
    32,     // mip 4 (  8^2)
    32,     // mip 5 (  4^2)
    32,     // mip 6 (  2^2)
};
// kPrefilterCBStride is defined as a class-static in SkyIBLPass.h so the
// PrefilterCBPool wrapper can size itself at compile time.

struct alignas(16) PrefilterConstants
{
    uint32_t faceSize;
    uint32_t sampleCount;
    float    roughness;
    uint32_t sourceMipCount;
    uint32_t faceIndex;    // temporal: which cube face this dispatch writes
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
};

// AtmosphereConstants is now declared as a nested type in SkyIBLPass.h so the
// FrameCB<AtmosphereConstants> member can be instantiated at class-definition
// time. The local copy here is intentionally removed to avoid ODR drift.

struct alignas(16) LutDimConstants
{
    uint32_t lutWidth;
    uint32_t lutHeight;
    uint32_t _pad0;
    uint32_t _pad1;
};

struct alignas(16) SkyViewConstants
{
    float    sunDir[3];         float cameraAltitudeKm;
    uint32_t lutWidth;
    uint32_t lutHeight;
    uint32_t _pad0;
    uint32_t _pad1;
};

struct alignas(16) AerialConstants
{
    float    sunDir[3];          float cameraAltitudeKm;
    float    sunColor[3];        float maxDistKm;
    float    invViewProj[16];
    float    cameraPosWorld[3];  float _pad0;
    float    cameraForward[3];   float _pad1;
    uint32_t lutW;
    uint32_t lutH;
    uint32_t lutD;
    uint32_t _pad2;
};

// kLutCBStride is a class-static in SkyIBLPass.h (sized for AerialConstants).
static constexpr uint32_t kLutCBSlotTrans  = 0;
static constexpr uint32_t kLutCBSlotMS     = 1;
static constexpr uint32_t kLutCBSlotSV     = 2;
static constexpr uint32_t kLutCBSlotAtmo   = 3;
static constexpr uint32_t kLutCBSlotAerial = 4;

// ---------------------------------------------------------------------------
void SkyIBLPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SkySHProjection_CS,  RHI::ShaderStage::CS,
                         "SkySHProjection.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::SpecularPrefilter_CS, RHI::ShaderStage::CS,
                         "SpecularPrefilter.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::SkyAtmosphere_CS, RHI::ShaderStage::CS,
                         "SkyAtmosphere.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::AtmosphereTransmittance_CS, RHI::ShaderStage::CS,
                         "AtmosphereTransmittance.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::AtmosphereMultiScatter_CS, RHI::ShaderStage::CS,
                         "AtmosphereMultiScatter.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::AtmosphereSkyView_CS, RHI::ShaderStage::CS,
                         "AtmosphereSkyView.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::AerialPerspective_CS, RHI::ShaderStage::CS,
                         "AerialPerspective.cs.hlsl", "main");

    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SkySHProjection_CS);
        if (!cs) { LOG_ERROR("SkyIBLPass: SkySHProjection_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_pso))
            LOG_ERROR("SkyIBLPass: SH projection PSO failed");
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SpecularPrefilter_CS);
        if (!cs) { LOG_ERROR("SkyIBLPass: SpecularPrefilter_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_prefilterPSO))
            LOG_ERROR("SkyIBLPass: specular prefilter PSO failed");
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SkyAtmosphere_CS);
        if (!cs) { LOG_ERROR("SkyIBLPass: SkyAtmosphere_CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_atmospherePSO))
            LOG_ERROR("SkyIBLPass: atmosphere PSO failed");
    }
    auto makeCsPso = [&](ShaderID id, RHI::PipelineState& out, const char* tag)
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("SkyIBLPass: %s_CS not found", tag); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, out))
            LOG_ERROR("SkyIBLPass: %s PSO failed", tag);
    };
    makeCsPso(ShaderID::AtmosphereTransmittance_CS, m_transmittancePSO, "Transmittance");
    makeCsPso(ShaderID::AtmosphereMultiScatter_CS,  m_multiScatterPSO,  "MultiScatter");
    makeCsPso(ShaderID::AtmosphereSkyView_CS,       m_skyViewPSO,       "SkyView");
    makeCsPso(ShaderID::AerialPerspective_CS,       m_aerialPSO,        "AerialPerspective");

    // SH output buffer — 9 × float4 (.xyz = coefficient, .w = padding).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kSHCoeffCount * sizeof(float) * 4;
        bd.stride     = sizeof(float) * 4;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;

        float zeros[kSHCoeffCount * 4] = {};
        if (!gfx.CreateBuffer(bd, m_shBuffer, zeros))
        {
            LOG_ERROR("SkyIBLPass: SH buffer creation failed");
            return;
        }

        m_shUavHandle = gfx.GetBufferUAVGpuHandle(m_shBuffer);
        m_shSrvHandle = gfx.GetBufferSRVGpuHandle(m_shBuffer);
    }

    // SH constants CB.
    m_cb.Create(gfx, "SkyIBL.SHConstants");

    // Prefilter per-dispatch CB — one 256-byte slot for each (face, mip) pair
    // so the bootstrap full bake (6 faces × 7 mips = 42 slots) doesn't race
    // the GPU. Subsequent temporal updates only use 7 of those slots.
    // FrameCB ring eliminates the cross-frame race on the pool itself.
    m_prefilterCB.Create(gfx, "SkyIBL.PrefilterCB");

    // Atmosphere CB — single AtmosphereConstants per frame.
    m_atmosphereCB.Create(gfx, "SkyIBL.AtmosphereCB");

    // Shared LUT CB pool: 5 × 512-byte slots (Transmittance / MultiScatter /
    // SkyView / [unused Atmo] / Aerial). Stride is 512 because AerialConstants
    // (invViewProj + lots of floats) exceeds the 256-byte slot size.
    m_atmoLutCB.Create(gfx, "SkyIBL.AtmoLutCB");

    CreateSpecularCube(gfx);
    CreateAtmosphereCube(gfx);
    CreateAtmosphereLUTs(gfx);

    LOG_SUCCESS("SkyIBLPass: initialised (SH %u floats, specular %ux%u %u mips)",
                kSHCoeffCount * 4, kSpecularSize, kSpecularSize, kSpecularMips);
}

// ---------------------------------------------------------------------------
void SkyIBLPass::CreateSpecularCube(IGraphicsDevice& gfx)
{
    // TextureCube (6 array slices) with UAV + 7-mip chain.
    // RHI auto-creates TextureCube SRV (TEXTURECUBE misc flag) and per-mip
    // Texture2DArray UAVs (mip_levels > 1 + UAV flag).
    RHI::TextureDesc td{};
    td.width      = kSpecularSize;
    td.height     = kSpecularSize;
    td.array_size = 6;
    td.mip_levels = kSpecularMips;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.misc_flags = RHI::ResourceMiscFlag::TEXTURECUBE;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!gfx.CreateTexture(td, m_specTex))
    {
        LOG_ERROR("SkyIBLPass: specular cubemap create failed");
        return;
    }
    m_specState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_specSrvHandle = gfx.GetTextureSRVGpuHandle(m_specTex);
}

// ---------------------------------------------------------------------------
// TickTimeOfDay() removed — TOD pipeline lives in ECS (see TODSystems.h).
// Renderer pushes m_sunDir/m_sunColor (active body) via SetSunDir and the
// geometric sun via SetAtmosphereSun each frame.
// ---------------------------------------------------------------------------
uint64_t SkyIBLPass::ResolveSkyboxSrvHandle(uint64_t staticFallback) const
{
    if (m_skyboxSource == SkyboxSource::Atmosphere
        && m_atmosphereEnabled && m_atmosphereSrvHandle != 0)
        return m_atmosphereSrvHandle;
    return staticFallback;
}

// ---------------------------------------------------------------------------
void SkyIBLPass::CreateAtmosphereCube(IGraphicsDevice& gfx)
{
    RHI::TextureDesc td{};
    td.width      = kAtmosphereSize;
    td.height     = kAtmosphereSize;
    td.array_size = 6;
    td.mip_levels = 1;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.misc_flags = RHI::ResourceMiscFlag::TEXTURECUBE;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!gfx.CreateTexture(td, m_atmosphereTex))
    {
        LOG_ERROR("SkyIBLPass: atmosphere cubemap create failed");
        return;
    }
    m_atmosphereState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_atmosphereSrvHandle = gfx.GetTextureSRVGpuHandle(m_atmosphereTex);
}

// ---------------------------------------------------------------------------
void SkyIBLPass::DispatchAtmosphere(RHI::CommandList cl)
{
    if (!m_atmospherePSO.IsValid()) return;
    if (!m_atmosphereTex.IsValid()) return;

    IGraphicsDevice& gfx = *m_gfx;

    if (m_atmosphereState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_atmosphereTex, m_atmosphereState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_atmosphereState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    if (auto* slot = m_atmosphereCB.Current(gfx))
    {
        AtmosphereConstants c{};
        // Use the geometric-sun pair (colour=0 below horizon) so the sky
        // produces no scattering at night.
        c.sunDir[0] = m_atmosphereSunDir.x;
        c.sunDir[1] = m_atmosphereSunDir.y;
        c.sunDir[2] = m_atmosphereSunDir.z;
        c.sunColor[0] = m_atmosphereSunColor.x;
        c.sunColor[1] = m_atmosphereSunColor.y;
        c.sunColor[2] = m_atmosphereSunColor.z;
        c.faceSize         = kAtmosphereSize;
        c.cameraAltitudeKm = 0.5f;
        *slot = c;
    }

    gfx.BindComputePipelineState(m_atmospherePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmosphereCB.CurrentBuffer(gfx), cl);
    // t0 = SkyView LUT, t1 = Transmittance LUT (see SkyAtmosphere.cs.hlsl).
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_skyViewTex), cl);
    gfx.SetComputeDescriptorTable(2,     gfx.GetTextureSRVGpuHandle(m_transmittanceTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_atmosphereTex), cl);

    const uint32_t g = (kAtmosphereSize + 7) / 8;
    gfx.DispatchCompute(g, g, 6, cl);

    // UAV → pixel-SRV (maps to PIXEL|NON_PIXEL_SR in DX12) for subsequent SH
    // projection + prefilter dispatches, AND for SkyboxPass/LightingPass later.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_atmosphereTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_atmosphereState = RHI::ResourceState::SHADER_RESOURCE;
}

// ---------------------------------------------------------------------------
void SkyIBLPass::DispatchPrefilter(RHI::CommandList cl, uint64_t sourceSrv)
{
    if (!m_prefilterPSO.IsValid()) return;
    if (!m_specTex.IsValid())      return;
    if (sourceSrv == 0)            return;

    IGraphicsDevice& gfx = *m_gfx;

    if (m_specState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_specTex, m_specState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_specState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    gfx.BindComputePipelineState(m_prefilterPSO, cl);
    gfx.SetComputeDescriptorTable(kSRV0, sourceSrv, cl);

    // Temporal schedule: the first bake after toggle/startup does all 6 faces
    // so the IBL is immediately valid. Subsequent frames do ONLY ONE face,
    // rotating through the cube — 6x cheaper per-frame, with a 6-frame
    // convergence latency that is essentially invisible for a slowly-
    // changing atmospheric sky.
    const uint32_t firstFace   = m_prefilterNeedsFullBake ? 0u : m_prefilterFaceCycle;
    const uint32_t facesThisRun = m_prefilterNeedsFullBake ? 6u : 1u;

    for (uint32_t fi = 0; fi < facesThisRun; ++fi)
    {
        const uint32_t face = firstFace + fi;

        for (uint32_t mip = 0; mip < kSpecularMips; ++mip)
        {
            const uint32_t faceW = (std::max)(1u, kSpecularSize >> mip);
            const float    roughness = (kSpecularMips <= 1) ? 0.0f
                                      : float(mip) / float(kSpecularMips - 1);

            // Pack the per-dispatch CB (each (face, mip) pair gets a unique
            // 256-byte slot so no writes overlap the GPU's read inside this
            // frame; FrameCB ring handles cross-frame).
            if (auto* pool = m_prefilterCB.Current(gfx))
            {
                PrefilterConstants c{};
                c.faceSize       = faceW;
                c.sampleCount    = (mip < 7) ? kPrefilterSampleTable[mip] : 32u;
                c.roughness      = roughness;
                c.sourceMipCount = 1;
                c.faceIndex      = face;
                const uint32_t slot = fi * kSpecularMips + mip;
                uint8_t* dst = pool->bytes + slot * kPrefilterCBStride;
                std::memcpy(dst, &c, sizeof(c));
                gfx.SetComputeRootCBV(kCBSlot, m_prefilterCB.CurrentBuffer(gfx),
                                      slot * kPrefilterCBStride, cl);
            }

            gfx.SetComputeDescriptorTable(kUAV0,
                gfx.GetTextureMipUAVGpuHandle(m_specTex, mip), cl);

            const uint32_t gx = (faceW + 7) / 8;
            const uint32_t gy = (faceW + 7) / 8;
            // Single face per dispatch (z = 1); shader writes into the face
            // slice selected by PrefilterCB::faceIndex.
            gfx.DispatchCompute(gx, gy, 1, cl);

            gfx.PushBarrier(RHI::GPUBarrier::Memory(nullptr), cl);
        }
    }

    if (m_prefilterNeedsFullBake)
        m_prefilterNeedsFullBake = false;
    m_prefilterFaceCycle = (m_prefilterFaceCycle + 1) % 6;

    // Transition to pixel SRV for this frame's lighting.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_specTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_specState = RHI::ResourceState::SHADER_RESOURCE;

    m_lastBakedSrc = sourceSrv;
    m_specValid    = true;
}

// ---------------------------------------------------------------------------
// Helper to create a 2D R16G16B16A16_FLOAT texture with UAV + SRV views
// through the RHI. Output texture is left in UNORDERED_ACCESS state.
static bool CreateLut2D(IGraphicsDevice& gfx,
                        uint32_t w, uint32_t h,
                        RHI::Texture& outTex,
                        const char* tag)
{
    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    if (!gfx.CreateTexture(td, outTex))
    {
        LOG_ERROR("SkyIBLPass: %s LUT create failed", tag);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
void SkyIBLPass::CreateAtmosphereLUTs(IGraphicsDevice& gfx)
{
    CreateLut2D(gfx, kTransmittanceW, kTransmittanceH, m_transmittanceTex, "Transmittance");
    CreateLut2D(gfx, kMultiScatterW,  kMultiScatterH,  m_multiScatterTex,  "MultiScatter");
    CreateLut2D(gfx, kSkyViewW,       kSkyViewH,       m_skyViewTex,       "SkyView");

    // Aerial Perspective: 3D R16G16B16A16_FLOAT (32×32×32) with UAV + SRV.
    // Start in SHADER_RESOURCE so the lighting pass can sample zeros safely
    // on the first frame before DispatchAerialPerspective runs.
    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
    td.width      = kAerialW;
    td.height     = kAerialH;
    td.depth      = kAerialD;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;
    if (!gfx.CreateTexture(td, m_aerialTex))
    {
        LOG_ERROR("SkyIBLPass: Aerial Perspective 3D LUT creation failed");
        return;
    }
    m_aerialSrvHandle = gfx.GetTextureSRVGpuHandle(m_aerialTex);
}

// ---------------------------------------------------------------------------
void SkyIBLPass::DispatchAerialPerspective(RHI::CommandList cl)
{
    if (!m_aerialPSO.IsValid() || !m_aerialTex.IsValid()) return;
    if (!m_staticLutsBaked)                               return;

    IGraphicsDevice& gfx = *m_gfx;

    if (m_aerialState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_aerialTex, m_aerialState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_aerialState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    if (auto* pool = m_atmoLutCB.Current(gfx))
    {
        AerialConstants c{};
        c.sunDir[0] = m_atmosphereSunDir.x;
        c.sunDir[1] = m_atmosphereSunDir.y;
        c.sunDir[2] = m_atmosphereSunDir.z;
        c.sunColor[0] = m_atmosphereSunColor.x;
        c.sunColor[1] = m_atmosphereSunColor.y;
        c.sunColor[2] = m_atmosphereSunColor.z;
        c.cameraAltitudeKm = 0.5f;
        c.maxDistKm = kAerialMaxKm;
        for (int i = 0; i < 16; ++i) c.invViewProj[i] = m_invViewProj[i];
        c.cameraPosWorld[0] = m_cameraPosWorld.x;
        c.cameraPosWorld[1] = m_cameraPosWorld.y;
        c.cameraPosWorld[2] = m_cameraPosWorld.z;
        c.cameraForward[0] = m_cameraForwardWorld.x;
        c.cameraForward[1] = m_cameraForwardWorld.y;
        c.cameraForward[2] = m_cameraForwardWorld.z;
        c.lutW = kAerialW; c.lutH = kAerialH; c.lutD = kAerialD;
        std::memcpy(pool->bytes + kLutCBSlotAerial * kLutCBStride, &c, sizeof(c));
    }

    gfx.BindComputePipelineState(m_aerialPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB.CurrentBuffer(gfx),
                          kLutCBSlotAerial * kLutCBStride, cl);
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_transmittanceTex), cl);
    gfx.SetComputeDescriptorTable(2,     gfx.GetTextureSRVGpuHandle(m_multiScatterTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_aerialTex), cl);

    // 8×8 threadgroup in xy, z = depth slice count — dispatch (4, 4, kAerialD).
    const uint32_t gx = (kAerialW + 7) / 8;
    const uint32_t gy = (kAerialH + 7) / 8;
    gfx.DispatchCompute(gx, gy, kAerialD, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_aerialTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_aerialState = RHI::ResourceState::SHADER_RESOURCE;

    m_aerialValid = true;
}

// ---------------------------------------------------------------------------
void SkyIBLPass::BakeStaticLUTs(RHI::CommandList cl)
{
    if (m_staticLutsBaked)         return;
    if (!m_transmittancePSO.IsValid() || !m_multiScatterPSO.IsValid()) return;
    if (!m_transmittanceTex.IsValid() || !m_multiScatterTex.IsValid()) return;

    IGraphicsDevice& gfx = *m_gfx;

    // Upload CB slots for Transmittance and MultiScatter dims.
    if (auto* pool = m_atmoLutCB.Current(gfx))
    {
        LutDimConstants t{ kTransmittanceW, kTransmittanceH, 0, 0 };
        LutDimConstants m{ kMultiScatterW,  kMultiScatterH,  0, 0 };
        std::memcpy(pool->bytes + kLutCBSlotTrans * kLutCBStride, &t, sizeof(t));
        std::memcpy(pool->bytes + kLutCBSlotMS    * kLutCBStride, &m, sizeof(m));
    }

    // ---- Transmittance ------------------------------------------------------
    gfx.BindComputePipelineState(m_transmittancePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB.CurrentBuffer(gfx),
                          kLutCBSlotTrans * kLutCBStride, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_transmittanceTex), cl);
    gfx.DispatchCompute((kTransmittanceW + 7) / 8, (kTransmittanceH + 7) / 8, 1, cl);

    // Transition Transmittance → SRV so MultiScatter can read it.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_transmittanceTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_transmittanceState = RHI::ResourceState::SHADER_RESOURCE;

    // ---- MultiScatter -------------------------------------------------------
    // Paper's parallel-per-pixel layout: one 1×1×64 threadgroup per LUT texel,
    // so Dispatch = (lutW, lutH, 1). Each threadgroup spawns 64 sphere samples
    // and LDS-reduces to a single output pixel.
    gfx.BindComputePipelineState(m_multiScatterPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB.CurrentBuffer(gfx),
                          kLutCBSlotMS * kLutCBStride, cl);
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_transmittanceTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_multiScatterTex), cl);
    gfx.DispatchCompute(kMultiScatterW, kMultiScatterH, 1, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_multiScatterTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_multiScatterState = RHI::ResourceState::SHADER_RESOURCE;

    m_staticLutsBaked = true;
    LOG_SUCCESS("SkyIBLPass: Transmittance + MultiScatter LUTs baked");
}

// ---------------------------------------------------------------------------
void SkyIBLPass::DispatchSkyViewLUT(RHI::CommandList cl)
{
    if (!m_skyViewPSO.IsValid() || !m_skyViewTex.IsValid()) return;
    if (!m_staticLutsBaked)                                 return;

    IGraphicsDevice& gfx = *m_gfx;

    if (m_skyViewState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_skyViewTex, m_skyViewState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_skyViewState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    if (auto* pool = m_atmoLutCB.Current(gfx))
    {
        SkyViewConstants c{};
        c.sunDir[0] = m_atmosphereSunDir.x;
        c.sunDir[1] = m_atmosphereSunDir.y;
        c.sunDir[2] = m_atmosphereSunDir.z;
        c.cameraAltitudeKm = 0.5f; // hard-coded ground-level camera
        c.lutWidth  = kSkyViewW;
        c.lutHeight = kSkyViewH;
        std::memcpy(pool->bytes + kLutCBSlotSV * kLutCBStride, &c, sizeof(c));
    }

    gfx.BindComputePipelineState(m_skyViewPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB.CurrentBuffer(gfx),
                          kLutCBSlotSV * kLutCBStride, cl);
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_transmittanceTex), cl);
    // t1 space2 (root param 2) — MultiScatter LUT.
    gfx.SetComputeDescriptorTable(2,     gfx.GetTextureSRVGpuHandle(m_multiScatterTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_skyViewTex), cl);
    gfx.DispatchCompute((kSkyViewW + 7) / 8, (kSkyViewH + 7) / 8, 1, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_skyViewTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_skyViewState = RHI::ResourceState::SHADER_RESOURCE;
}

// ---------------------------------------------------------------------------
RHI::CommandList SkyIBLPass::Execute(RHI::CommandList cl)
{
    if (!m_pso.IsValid())      return cl;
    if (!m_shBuffer.IsValid()) return cl;

    // Pure static skybox mode — do NOTHING. No atmosphere, no SH projection,
    // no specular pre-filter. The lighting path falls back to the scene's
    // irradiance + radiance cubemaps directly (zero GPU cost here).
    if (!m_atmosphereEnabled)
        return cl;

    // ---- Dirty check: skip every per-frame dispatch when nothing changed ----
    // The atmosphere cube + SkyView LUT + SH coefficients are PURE functions
    // of the sun direction and sun intensity. If neither has moved since the
    // last bake, every downstream buffer already holds the correct result.
    // Gating this pass reduces the IBL cost to essentially 0 for paused TOD
    // and static scenes.
    const float cosDelta = DirectX::XMVectorGetX(
        DirectX::XMVector3Dot(
            DirectX::XMLoadFloat3(&m_sunDir),
            DirectX::XMLoadFloat3(&m_lastBakedSunDir)));
    const bool sunDirChanged = cosDelta < 0.99999f;     // ~ 0.25°
    const float curSunMag    = m_sunColor.x + m_sunColor.y + m_sunColor.z;
    const bool sunIntChanged = std::abs(curSunMag - m_lastBakedSunIntensity) > 1e-3f;

    const bool needCubeUpdate = m_prefilterNeedsFullBake || sunDirChanged || sunIntChanged;

    uint64_t sourceSrv = m_atmosphereSrvHandle;
    if (sourceSrv == 0) return cl;

    // ---- Fine-grained GPU profiling ----------------------------------------
    // Each sub-step below is wrapped in its own Begin/EndTimestamp pair so
    // the editor's GPU Profiler panel lists per-step costs (SkyIBL.SkyView /
    // .Atmosphere / .SH / .Prefilter) instead of one opaque number.
    IGraphicsDevice& gfx = *m_gfx;
    auto pBegin = [&](const char* name) -> uint32_t {
        return gfx.BeginGPUTimestamp(cl, name);
    };
    auto pEnd = [&](uint32_t r) {
        gfx.EndGPUTimestamp(cl, r);
    };

    // ---- Hillaire procedural sky (only when sun state actually changed) -----
    if (needCubeUpdate)
    {
        BakeStaticLUTs(cl);          // one-shot, early-outs after first frame

        {
            uint32_t r = pBegin("SkyIBL.SkyViewLUT");
            DispatchSkyViewLUT(cl);
            pEnd(r);
        }

        if (m_aerialCompositeEnabled)
        {
            uint32_t r = pBegin("SkyIBL.AerialPerspective");
            DispatchAerialPerspective(cl);
            pEnd(r);
        }

        {
            uint32_t r = pBegin("SkyIBL.AtmosphereCube");
            DispatchAtmosphere(cl);
            pEnd(r);
        }

        m_lastBakedSunDir        = m_sunDir;
        m_lastBakedSunIntensity  = curSunMag;
        m_framesSinceCubeUpdate  = 0;
    }
    else
    {
        // Still invoke BakeStaticLUTs so the one-shot bake can run on the
        // very first frame even if the sun happened to start at identity.
        BakeStaticLUTs(cl);
        if (m_framesSinceCubeUpdate != 0xFFFFFFFFu)
            m_framesSinceCubeUpdate++;
    }

    // ---- SH projection — only when the atmosphere cube was re-baked --------
    // SH reads the atmosphere cube; stale cube → stale SH. Skipping when
    // nothing changed saves the LDS-reduction dispatch (~0.05 ms).
    if (needCubeUpdate)
    {
        uint32_t r = pBegin("SkyIBL.SHProjection");

        if (auto* slot = m_cb.Current(gfx))
        {
            SkyIBLPass::SHConstants c{};
            c.sampleCount = kSHSampleCount;
            *slot = c;
        }

        if (m_shState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_shBuffer, m_shState,
                            RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_shState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_pso, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
        gfx.SetComputeDescriptorTable(kSRV0, sourceSrv, cl);
        gfx.SetComputeDescriptorTable(kUAV0, m_shUavHandle, cl);
        gfx.DispatchCompute(1, 1, 1, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_shBuffer), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_shBuffer, RHI::ResourceState::UNORDERED_ACCESS,
                        RHI::ResourceState::SHADER_RESOURCE), cl);
        m_shState = RHI::ResourceState::SHADER_RESOURCE;
        m_shValid = true;

        pEnd(r);
    }

    // ---- Specular prefilter — temporal, runs until all 6 faces have caught
    // up with the latest atmosphere cube (~6 frames after a change). If the
    // sun hasn't moved long enough, every face already reflects the current
    // cube and we can skip the ~0.4 ms prefilter dispatch entirely.
    const bool needPrefilter = m_prefilterNeedsFullBake
                             || (m_framesSinceCubeUpdate < 6);
    if (needPrefilter)
    {
        // Label the bootstrap (all 6 faces) and the temporal (1 face/frame)
        // runs differently so the editor shows the ~6x cost spike on mode
        // toggle versus the steady-state temporal cost.
        const char* name = m_prefilterNeedsFullBake ? "SkyIBL.Prefilter(Bootstrap 6f)"
                                                    : "SkyIBL.Prefilter(1f)";
        uint32_t r = pBegin(name);
        DispatchPrefilter(cl, sourceSrv);
        pEnd(r);
    }

    return cl;
}
