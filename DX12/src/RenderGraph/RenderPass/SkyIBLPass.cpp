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
static constexpr uint32_t kPrefilterCBStride = 256; // D3D12 CBV alignment

struct alignas(16) SHConstants
{
    uint32_t sampleCount;
    uint32_t _pad[3];
};

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

struct alignas(16) AtmosphereConstants
{
    float    sunDir[3];    float _pad0;
    float    sunColor[3];  uint32_t faceSize;
    float    cameraAltitudeKm;
    float    _pad1;
    float    _pad2;
    float    _pad3;
};

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

static constexpr uint32_t kLutCBStride   = 512; // larger for AerialConstants
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
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = (sizeof(SHConstants) + 255) & ~255u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_cb))
            m_cbMapped = gfx.MapBuffer(m_cb);
    }

    // Prefilter per-dispatch CB — one 256-byte slot for each (face, mip) pair
    // so the bootstrap full bake (6 faces × 7 mips = 42 slots) doesn't race
    // the GPU. Subsequent temporal updates only use 7 of those slots.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kPrefilterCBStride * kSpecularMips * 6;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_prefilterCB))
            m_prefilterCBMapped = gfx.MapBuffer(m_prefilterCB);
    }

    // Atmosphere CB — single 256-byte slot.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = 256;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_atmosphereCB))
            m_atmosphereCBMapped = gfx.MapBuffer(m_atmosphereCB);
    }

    // Shared LUT CB pool: 5 × 512-byte slots (Transmittance / MultiScatter /
    // SkyView / Atmosphere / Aerial). Stride is 512 because AerialConstants
    // (invViewProj + lots of floats) exceeds the 256-byte slot size.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kLutCBStride * 5;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_atmoLutCB))
            m_atmoLutCBMapped = gfx.MapBuffer(m_atmoLutCB);
    }

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
bool SkyIBLPass::TickTimeOfDay(float deltaSeconds)
{
    if (!m_timeOfDayEnabled) return false;

    if (m_timeSpeed != 0.0f)
    {
        m_timeOfDay += m_timeSpeed * deltaSeconds;
        m_timeOfDay -= std::floor(m_timeOfDay); // wrap to [0, 1)
    }

    // Convert normalised time-of-day to hour angle (H = 0 at solar noon,
    // +π at midnight). timeOfDay=0.5 means noon (sun at zenith for lat=0).
    const float H   = (m_timeOfDay - 0.5f) * (2.0f * 3.14159265f);
    const float lat = m_latitudeRad;

    // Solar declination: treat as zero (equinox) for simplicity — the sun
    // traces a great circle through the zenith at the equator on equinox.
    // Hour angle rotates about the world Y-axis; latitude tilts the plane.
    const float cosH = std::cos(H);
    const float sinH = std::sin(H);
    const float cosL = std::cos(lat);
    const float sinL = std::sin(lat);

    // Sun direction in a local frame where Y is up (zenith at noon).
    // Standard equatorial-to-horizontal transform (declination δ = 0):
    //   altitude α:  sin(α) = sin(φ) sin(δ) + cos(φ) cos(δ) cos(H)
    //                       = cos(φ) cos(H)
    //   azimuth A:   uses the other coordinates; here we build the vector directly.
    DirectX::XMFLOAT3 dir;
    dir.x = -std::cos(0.0f) * sinH;           // east–west: +x east at sunrise side
    dir.y = cosL * cosH + sinL * 0.0f;        // zenith component
    dir.z = -sinL * cosH + cosL * 0.0f;       // north–south
    // Normalise for safety.
    const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }

    // ---- Day / night swap ----------------------------------------------------
    // When the geometric sun drops below horizon, the MOON takes over as the
    // directional light. The moon sits exactly opposite the sun in the sky
    // (-dir). All downstream consumers (LightCB, shadows, fog) just see this
    // as "the active sun" — no per-consumer special case. The Skybox pass
    // reads IsMoonActive() to swap the analytic disk for a textured moon.
    const bool sunBelowHorizon = (dir.y < 0.0f);
    m_isMoon = sunBelowHorizon;

    // Skybox-only moon-disk state — always compute moon position (= -sunDir).
    // Early-fade: disk starts fading around +7° and fully disappears at +0.6°
    // (matches the shader's smoothstep(0.01, 0.12, moonDir.y)). We gate the
    // CB upload at the low-end threshold so there's no per-frame waste once
    // the moon has set.
    m_moonDir = { -dir.x, -dir.y, -dir.z };
    m_moonDiskVisible = (m_moonDir.y > 0.01f);
    // Moon disk tint stays constant (cool blue-white) so it's clearly visible
    // throughout its arc — intensity fade happens in the pixel shader via the
    // horizon mask, not via this colour. Small multiplier — the disk should
    // read as a bright celestial body but not bloom out the night sky.
	float moonScale = 0.1f; // tweak this to make the moon brighter or dimmer relative to the sun
    m_moonColor = { 0.55f * moonScale, 0.70f * moonScale, 1.00f * moonScale };

    // The atmosphere / aerial-perspective shaders sample the GEOMETRIC sun.
    // Below horizon we force its colour to zero so Rayleigh / Mie scattering
    // stops contributing — sky goes black, no blue daylight dome at night.
    m_atmosphereSunDir = dir;
    if (sunBelowHorizon)
    {
        m_atmosphereSunColor = { 0.0f, 0.0f, 0.0f };
    }
    else
    {
        const float elev      = dir.y;
        const float daylight  = std::max(0.0f, elev);
        const float tw        = std::exp(-std::max(0.0f, -elev) * 8.0f);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * (0.05f + 0.95f * daylight) * tw
                              * m_sunIntensityScale;
        const float warmth    = 1.0f - std::min(1.0f, daylight * 2.0f);
        m_atmosphereSunColor = {
            intensity * 1.00f,
            intensity * (1.00f - 0.30f * warmth),
            intensity * (1.00f - 0.55f * warmth) };
    }

    // Active light direction flips hard at horizon — sun and moon point in
    // opposite directions so there is no meaningful "blended" direction. To
    // keep that flip invisible we use NON-OVERLAPPING ramps below: both sunT
    // and moonT hit zero at dir.y == 0, so total intensity is zero at the
    // instant of the direction switch.
    if (sunBelowHorizon)
    {
        m_sunDir.x = -dir.x;
        m_sunDir.y = -dir.y;
        m_sunDir.z = -dir.z;
    }
    else
    {
        m_sunDir = dir;
    }

    // Cross-fade weights. kBand = 0.05 ≈ 2.9° — narrow enough that the twilight
    // dip isn't obvious, wide enough that any intensity & direction change is
    // spread across several frames of a typical time-of-day cycle.
    //
    //   y >= +kBand : sunT = 1, moonT = 0     → pure sun
    //   y == 0      : sunT = 0, moonT = 0     → both dark, direction flip safe
    //   y <= -kBand : sunT = 0, moonT = 1     → pure moon
    auto smoothstep01 = [](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float kBand = 0.05f;
    const float sunT  = smoothstep01( dir.y / kBand);
    const float moonT = smoothstep01(-dir.y / kBand);

    // --- Sun colour model (original formula, clamped to y >= 0) ---------------
    DirectX::XMFLOAT3 sunCol { 0, 0, 0 };
    {
        const float elev      = std::max(0.0f, dir.y);
        const float tw        = std::exp(-std::max(0.0f, -dir.y) * 8.0f);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * (0.05f + 0.95f * elev) * tw
                              * m_sunIntensityScale;
        const float warmth    = 1.0f - std::min(1.0f, elev * 2.0f);
        sunCol.x = intensity * (1.00f);
        sunCol.y = intensity * (1.00f - 0.30f * warmth);
        sunCol.z = intensity * (1.00f - 0.55f * warmth);
    }

    // --- Moon colour model (original formula, using the moon's elevation) -----
    DirectX::XMFLOAT3 moonCol { 0, 0, 0 };
    {
        const float moonElev  = -dir.y;                // moon = -sun
        const float moonlight = std::max(0.0f, moonElev);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * m_moonIntensityScale
                              * (0.75f + 0.25f * moonlight)
                              * m_sunIntensityScale;
        moonCol.x = intensity * 0.55f;
        moonCol.y = intensity * 0.70f;
        moonCol.z = intensity * 1.00f;
    }

    m_sunColor.x = sunCol.x * sunT + moonCol.x * moonT;
    m_sunColor.y = sunCol.y * sunT + moonCol.y * moonT;
    m_sunColor.z = sunCol.z * sunT + moonCol.z * moonT;

    return true;
}

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

    if (m_atmosphereCBMapped)
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
        std::memcpy(m_atmosphereCBMapped, &c, sizeof(c));
    }

    gfx.BindComputePipelineState(m_atmospherePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmosphereCB, cl);
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
            // 256-byte slot so no writes overlap the GPU's read).
            if (m_prefilterCBMapped)
            {
                PrefilterConstants c{};
                c.faceSize       = faceW;
                c.sampleCount    = (mip < 7) ? kPrefilterSampleTable[mip] : 32u;
                c.roughness      = roughness;
                c.sourceMipCount = 1;
                c.faceIndex      = face;
                const uint32_t slot = fi * kSpecularMips + mip;
                uint8_t* dst = static_cast<uint8_t*>(m_prefilterCBMapped)
                             + slot * kPrefilterCBStride;
                std::memcpy(dst, &c, sizeof(c));
                gfx.SetComputeRootCBV(kCBSlot, m_prefilterCB, slot * kPrefilterCBStride, cl);
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

    if (m_atmoLutCBMapped)
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
        std::memcpy(static_cast<uint8_t*>(m_atmoLutCBMapped) + kLutCBSlotAerial * kLutCBStride,
                    &c, sizeof(c));
    }

    gfx.BindComputePipelineState(m_aerialPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB, kLutCBSlotAerial * kLutCBStride, cl);
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
    if (m_atmoLutCBMapped)
    {
        LutDimConstants t{ kTransmittanceW, kTransmittanceH, 0, 0 };
        LutDimConstants m{ kMultiScatterW,  kMultiScatterH,  0, 0 };
        std::memcpy(static_cast<uint8_t*>(m_atmoLutCBMapped) + kLutCBSlotTrans * kLutCBStride, &t, sizeof(t));
        std::memcpy(static_cast<uint8_t*>(m_atmoLutCBMapped) + kLutCBSlotMS    * kLutCBStride, &m, sizeof(m));
    }

    // ---- Transmittance ------------------------------------------------------
    gfx.BindComputePipelineState(m_transmittancePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB, kLutCBSlotTrans * kLutCBStride, cl);
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
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB, kLutCBSlotMS * kLutCBStride, cl);
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

    if (m_atmoLutCBMapped)
    {
        SkyViewConstants c{};
        c.sunDir[0] = m_atmosphereSunDir.x;
        c.sunDir[1] = m_atmosphereSunDir.y;
        c.sunDir[2] = m_atmosphereSunDir.z;
        c.cameraAltitudeKm = 0.5f; // hard-coded ground-level camera
        c.lutWidth  = kSkyViewW;
        c.lutHeight = kSkyViewH;
        std::memcpy(static_cast<uint8_t*>(m_atmoLutCBMapped) + kLutCBSlotSV * kLutCBStride,
                    &c, sizeof(c));
    }

    gfx.BindComputePipelineState(m_skyViewPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB, kLutCBSlotSV * kLutCBStride, cl);
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

        if (m_cbMapped)
        {
            SHConstants c{};
            c.sampleCount = kSHSampleCount;
            std::memcpy(m_cbMapped, &c, sizeof(c));
        }

        if (m_shState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_shBuffer, m_shState,
                            RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_shState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_pso, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cb, cl);
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
