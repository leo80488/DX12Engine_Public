#include "Graphics/PSOCache.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>

// ---------------------------------------------------------------------------
// PSODesc::Hash
// FNV-1a 64-bit with 8-byte-chunked mixing (~8× the byte-at-a-time version).
// Called once per draw packet per pass; byte-level FNV showed up at ~7% CPU
// in profiling before this change.
// ---------------------------------------------------------------------------
uint64_t PSODesc::Hash() const noexcept
{
    constexpr uint64_t kPrime = 1099511628211ull;
    // Root-signature / hashing-scheme version — bump when you add/remove/move
    // root parameters OR change this hash function. Mixing it in invalidates
    // every cached PSO name on the next run so the disk pso_cache.bin
    // self-purges when state-binding semantics or hash structure shift.
    //   v3 → v4: switched from raw-byte hashing of RHI state structs to
    //            per-field hashing. Old hashing pulled in padding bytes which
    //            were not zero-initialised in stack-allocated PSODescs; that
    //            made cache_key non-deterministic across runs and caused
    //            pso_cache.bin to grow on every launch.
    //   v4 → v5: added graphics root param 46 (t41 space0 — point-light cube
    //            shadow atlas, PointShadowPass). The old 46-param sig is a
    //            forward-compatible prefix so solid PSOs from a stale cache
    //            still draw, but feeding a stale (old-sig) PSO to the PSO
    //            library's LoadGraphicsPipeline with the new sig crashes the
    //            debug layer — bumping this purges those entries by name.
    constexpr uint64_t kRootSigVersion = 5ull;

    uint64_t h = 14695981039346656037ull;
    h ^= kRootSigVersion; h *= kPrime;

    // Per-byte FNV-1a — robust and obviously deterministic. No raw-struct
    // bulk mixing here on purpose; padding bytes are the enemy.
    auto mixU8  = [&h, kPrime](uint8_t v)  { h ^= v; h *= kPrime; };
    auto mixU32 = [&](uint32_t v) {
        mixU8(static_cast<uint8_t>(v));
        mixU8(static_cast<uint8_t>(v >>  8));
        mixU8(static_cast<uint8_t>(v >> 16));
        mixU8(static_cast<uint8_t>(v >> 24));
    };
    auto mixI32  = [&](int32_t  v) { mixU32(static_cast<uint32_t>(v)); };
    auto mixF32  = [&](float    v) { uint32_t u; std::memcpy(&u, &v, 4); mixU32(u); };
    auto mixBool = [&](bool     v) { mixU8(v ? 1 : 0); };
    auto mixEnum = [&](auto     v) { mixU32(static_cast<uint32_t>(v)); };

    // ---- Top-level scalars -------------------------------------------------
    mixEnum(vsID);
    mixEnum(psID);
    mixU32(perm.bits);
    mixU8(static_cast<uint8_t>(inputLayout));
    mixEnum(topology);
    mixEnum(dsvFormat);
    mixU32(rtvCount);
    mixU32(sampleCount);
    for (uint32_t i = 0; i < rtvCount && i < 8; ++i)
        mixEnum(rtvFormats[i]);

    // ---- RasterizerState ---------------------------------------------------
    mixEnum(rs.fill_mode);
    mixEnum(rs.cull_mode);
    mixBool(rs.front_counter_clockwise);
    mixI32 (rs.depth_bias);
    mixF32 (rs.depth_bias_clamp);
    mixF32 (rs.slope_scaled_depth_bias);
    mixBool(rs.depth_clip_enable);
    mixBool(rs.multisample_enable);
    mixBool(rs.antialiased_line_enable);
    mixBool(rs.conservative_rasterization_enable);
    mixU32 (rs.forced_sample_count);

    // ---- DepthStencilState -------------------------------------------------
    mixBool(dss.depth_enable);
    mixEnum(dss.depth_write_mask);
    mixEnum(dss.depth_func);
    mixBool(dss.stencil_enable);
    mixU8  (dss.stencil_read_mask);
    mixU8  (dss.stencil_write_mask);
    auto mixOp = [&](const RHI::DepthStencilState::DepthStencilOp& op) {
        mixEnum(op.stencil_fail_op);
        mixEnum(op.stencil_depth_fail_op);
        mixEnum(op.stencil_pass_op);
        mixEnum(op.stencil_func);
    };
    mixOp  (dss.front_face);
    mixOp  (dss.back_face);
    mixBool(dss.depth_bounds_test_enable);

    // ---- BlendState (8 RT slots) -------------------------------------------
    mixBool(bs.alpha_to_coverage_enable);
    mixBool(bs.independent_blend_enable);
    for (uint32_t i = 0; i < 8; ++i)
    {
        const auto& rt = bs.render_target[i];
        mixBool(rt.blend_enable);
        mixEnum(rt.src_blend);
        mixEnum(rt.dest_blend);
        mixEnum(rt.blend_op);
        mixEnum(rt.src_blend_alpha);
        mixEnum(rt.dest_blend_alpha);
        mixEnum(rt.blend_op_alpha);
        mixEnum(rt.render_target_write_mask);
    }

    return h;
}

// ---------------------------------------------------------------------------
// BuildInputLayout
// ---------------------------------------------------------------------------
RHI::InputLayout PSOCache::BuildInputLayout(InputLayoutType type)
{
    RHI::InputLayout il;
    switch (type)
    {
    case InputLayoutType::StaticMesh:
        il.elements = {
            { "POSITION", 0, RHI::Format::R32G32B32_FLOAT, 0,
              RHI::InputLayout::APPEND_ALIGNED_ELEMENT,
              RHI::InputClassification::PER_VERTEX_DATA },
            { "NORMAL",   0, RHI::Format::R32G32B32_FLOAT, 0,
              RHI::InputLayout::APPEND_ALIGNED_ELEMENT,
              RHI::InputClassification::PER_VERTEX_DATA },
            { "COLOR",    0, RHI::Format::R32G32B32_FLOAT, 0,
              RHI::InputLayout::APPEND_ALIGNED_ELEMENT,
              RHI::InputClassification::PER_VERTEX_DATA },
        };
        break;
    case InputLayoutType::None:
    default:
        break;
    }
    return il;
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void PSOCache::Init(IGraphicsDevice& gfx, ShaderLibrary& shaderLib)
{
    m_gfx       = &gfx;
    m_shaderLib = &shaderLib;
    m_cache.reserve(64);
}

void PSOCache::Shutdown()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_cache.clear();
    m_gfx       = nullptr;
    m_shaderLib = nullptr;
}

void PSOCache::Clear()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_cache.clear();
    m_failedPSOs.clear();   // hot-reload retries previously-broken PSOs
}

// ---------------------------------------------------------------------------
// GetOrCreate
// ---------------------------------------------------------------------------
const RHI::PipelineState* PSOCache::GetOrCreate(const PSODesc& desc)
{
    const uint64_t key = desc.Hash();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end())
            return &it->second;
        // Already-known-broken PSO — skip the rebuild so the per-frame draw
        // doesn't re-run CreateNew and re-log "PS not found" thousands of
        // times. ClearCaches (hot-reload path) drops this set so a fixed
        // shader gets rebuilt naturally.
        if (m_failedPSOs.find(key) != m_failedPSOs.end())
            return nullptr;
    }

    RHI::PipelineState pso = CreateNew(desc);
    if (!pso.IsValid())
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_failedPSOs.insert(key);
        return nullptr;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    auto [it, inserted] = m_cache.emplace(key, std::move(pso));
    return &it->second;
}

// ---------------------------------------------------------------------------
// CreateNew
// ---------------------------------------------------------------------------
RHI::PipelineState PSOCache::CreateNew(const PSODesc& desc)
{
    if (!m_gfx || !m_shaderLib)
    {
        LOG_ERROR("PSOCache: not initialized");
        return {};
    }

    const RHI::Shader* vs = m_shaderLib->GetShader(desc.vsID, desc.perm);
    const RHI::Shader* ps = m_shaderLib->GetShader(desc.psID, desc.perm);
    if (!vs || !vs->IsValid())
    { LOG_ERROR("PSOCache: VS not found (id=%u, perm=0x%08X)", static_cast<uint32_t>(desc.vsID), desc.perm.bits); return {}; }
    if (!ps || !ps->IsValid())
    { LOG_ERROR("PSOCache: PS not found (id=%u, perm=0x%08X)", static_cast<uint32_t>(desc.psID), desc.perm.bits); return {}; }

    RHI::PipelineStateDesc psd;
    psd.layout     = nullptr;
    psd.vs         = vs;
    psd.ps         = ps;
    psd.rs         = &desc.rs;
    psd.dss        = &desc.dss;
    psd.bs         = &desc.bs;
    psd.il         = nullptr;  // PVF: input layout always null — no exceptions
    psd.rtv_count  = desc.rtvCount;
    psd.dsv_format = desc.dsvFormat;
    psd.sample_count = desc.sampleCount;
    psd.pt           = desc.topology;
    for (uint32_t i = 0; i < desc.rtvCount && i < 8; ++i)
        psd.rtv_formats[i] = desc.rtvFormats[i];

    psd.cache_key = desc.Hash();  // stable ShaderID-based key for PSO disk library

    RHI::PipelineState outPSO;
    if (!m_gfx->CreatePipelineState(psd, outPSO))
    {
        LOG_ERROR("PSOCache: CreatePipelineState failed (vsID=%u, psID=%u, perm=0x%08X)",
                  static_cast<uint32_t>(desc.vsID),
                  static_cast<uint32_t>(desc.psID),
                  desc.perm.bits);
        return {};
    }
    return outPSO;
}
