#include "RenderGraph/RenderPass/GlassShatterPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace DirectX;

// Compute root signature slots (shared with every compute pass, space2).
static constexpr uint32_t kCBSlot = 0;
static constexpr uint32_t kSRV0   = 1;  // t0
static constexpr uint32_t kSRV1   = 2;  // t1
static constexpr uint32_t kSRV2   = 3;  // t2
static constexpr uint32_t kUAV0   = 4;  // u0
static constexpr uint32_t kUAV1   = 5;  // u1

// ============================================================================
// CPU Voronoi mesh generation — Sutherland-Hodgman clipping
// ============================================================================
namespace
{
    using Vec2 = XMFLOAT2;

    inline Vec2 V(float x, float y) { return {x, y}; }
    inline Vec2 Sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    inline float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
    inline float Length(Vec2 a) { return std::sqrt(Dot(a, a)); }
    inline Vec2 Normalize(Vec2 a)
    {
        const float l = Length(a);
        return l > 1e-8f ? Vec2{a.x / l, a.y / l} : Vec2{0, 0};
    }

    // Clip polygon `verts` against the half-plane { p : dot(n, p) <= d }.
    // Output is in CCW order if input was. Empty if entire polygon is outside.
    void ClipHalfPlane(std::vector<Vec2>& io, Vec2 n, float d)
    {
        if (io.empty()) return;
        std::vector<Vec2> out;
        out.reserve(io.size() + 2);
        const size_t k = io.size();
        for (size_t i = 0; i < k; ++i)
        {
            const Vec2  cur  = io[i];
            const Vec2  prev = io[(i + k - 1) % k];
            const float dCur = Dot(n, cur);
            const float dPrv = Dot(n, prev);
            const bool  curIn = (dCur <= d);
            const bool  prvIn = (dPrv <= d);

            if (curIn)
            {
                if (!prvIn)
                {
                    const float t = (d - dPrv) / (dCur - dPrv);
                    out.push_back({prev.x + (cur.x - prev.x) * t,
                                   prev.y + (cur.y - prev.y) * t});
                }
                out.push_back(cur);
            }
            else if (prvIn)
            {
                const float t = (d - dPrv) / (dCur - dPrv);
                out.push_back({prev.x + (cur.x - prev.x) * t,
                               prev.y + (cur.y - prev.y) * t});
            }
        }
        io.swap(out);
    }
}

// ============================================================================
// Init
// ============================================================================
void GlassShatterPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GlassShatterInit_CS,      RHI::ShaderStage::CS,
                         "GlassShatterInit.cs.hlsl",      "CSMain");
    m_shaderLib.Register(ShaderID::GlassShatterSimulate_CS,  RHI::ShaderStage::CS,
                         "GlassShatterSimulate.cs.hlsl",  "CSMain");
    m_shaderLib.Register(ShaderID::GlassShatterComposite_CS, RHI::ShaderStage::CS,
                         "GlassShatterComposite.cs.hlsl", "CSMain");

    auto compile = [&](ShaderID id, RHI::PipelineState& outPSO, const char* name)
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("GlassShatterPass: %s shader load failed", name); return false; }
        RHI::PipelineStateDesc pd{};
        pd.cs = cs;
        if (!gfx.CreatePipelineState(pd, outPSO))
        {
            LOG_ERROR("GlassShatterPass: %s PSO creation failed", name);
            return false;
        }
        return true;
    };
    if (!compile(ShaderID::GlassShatterInit_CS,      m_initPSO,      "Init"))      return;
    if (!compile(ShaderID::GlassShatterSimulate_CS,  m_simulatePSO,  "Simulate"))  return;
    if (!compile(ShaderID::GlassShatterComposite_CS, m_compositePSO, "Composite")) return;

    GenerateVoronoiMesh();

    // Per-frame CB.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = (sizeof(ShatterCB) + 255u) & ~255u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_paramsCB))
            m_paramsCBMapped = gfx.MapBuffer(m_paramsCB);
    }

    LOG_SUCCESS("GlassShatterPass: initialized (%u shards)", m_shardCount);
}

// ============================================================================
// Voronoi mesh — generate one fixed pattern at startup.
//   - jittered grid of seeds in UV [0,1]
//   - each seed clipped against perpendicular bisectors with all others
//   - result is the seed's Voronoi cell (convex polygon in UV)
//   - cell edges stored as (normal, d) in shard-local UV space (centroid-rel)
// ============================================================================
void GlassShatterPass::GenerateVoronoiMesh()
{
    if (!m_gfx) return;

    // ---- Seed points ----------------------------------------------------
    // 9x9 jittered grid → 81 seeds. Inside-square margin (5% inset) keeps
    // every seed strictly interior so its Voronoi cell isn't an unbounded
    // half-plane. Jitter range 0..1 within each cell, scaled 0.6 so seeds
    // don't cluster too tightly across boundaries.
    constexpr int kGridN = 9;
    std::vector<Vec2> seeds;
    seeds.reserve(kGridN * kGridN);

    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    for (int j = 0; j < kGridN; ++j)
    for (int i = 0; i < kGridN; ++i)
    {
        const float jx = uni(rng);
        const float jy = uni(rng);
        const float u  = (i + 0.2f + 0.6f * jx) / float(kGridN);
        const float v  = (j + 0.2f + 0.6f * jy) / float(kGridN);
        seeds.push_back(V(u, v));
    }

    // ---- Per-seed clip & edge extraction ---------------------------------
    std::vector<ShardMeta> meta;
    std::vector<ShardEdge> edges;
    meta.reserve(seeds.size());

    for (size_t s = 0; s < seeds.size(); ++s)
    {
        std::vector<Vec2> cell = { V(0,0), V(1,0), V(1,1), V(0,1) };  // CCW unit square

        for (size_t o = 0; o < seeds.size(); ++o)
        {
            if (o == s) continue;
            const Vec2  diff   = Sub(seeds[o], seeds[s]);
            const float dl     = Length(diff);
            if (dl < 1e-6f) continue;
            const Vec2  nrm    = { diff.x / dl, diff.y / dl };  // points from s toward o
            const Vec2  midPt  = { (seeds[s].x + seeds[o].x) * 0.5f,
                                   (seeds[s].y + seeds[o].y) * 0.5f };
            const float plane  = Dot(nrm, midPt);
            ClipHalfPlane(cell, nrm, plane);
            if (cell.empty()) break;
        }
        if (cell.size() < 3) continue;

        // Centroid (average — close enough for nearly-regular cells).
        Vec2 c = {0, 0};
        for (const auto& v : cell) { c.x += v.x; c.y += v.y; }
        c.x /= cell.size(); c.y /= cell.size();

        // Edges in shard-local UV space (relative to centroid). Normal points
        // outward (CCW polygon ⇒ right-hand normal of edge).
        const uint32_t edgeStart = static_cast<uint32_t>(edges.size());
        const size_t   k         = cell.size();
        for (size_t i = 0; i < k; ++i)
        {
            const Vec2 v0 = Sub(cell[i],            c);
            const Vec2 v1 = Sub(cell[(i + 1) % k],  c);
            const Vec2 e  = Sub(v1, v0);
            // Right-hand outward normal for CCW polygon: rotate e by -90°.
            // (e.x, e.y) → (e.y, -e.x). Normalised.
            const Vec2 nrm = Normalize({ e.y, -e.x });
            const float dPlane = Dot(nrm, v0);  // pixel inside ⇔ dot(n, p) <= dPlane
            edges.push_back({ nrm, dPlane, 0.0f });
        }

        ShardMeta m;
        m.centroidUV  = c;
        m.edgeOffset  = edgeStart;
        m.edgeCount   = static_cast<uint32_t>(k);
        meta.push_back(m);

        if (meta.size() >= kTargetShards) break;
    }

    m_shardCount = static_cast<uint32_t>(meta.size());
    if (m_shardCount == 0)
    {
        LOG_ERROR("GlassShatterPass: Voronoi generation produced 0 shards");
        return;
    }

    // ---- Upload meta buffer (UPLOAD heap, written once) -----------------
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(meta.size()) * sizeof(ShardMeta);
        bd.stride     = sizeof(ShardMeta);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        if (m_gfx->CreateBuffer(bd, m_metaBuffer))
        {
            void* p = m_gfx->MapBuffer(m_metaBuffer);
            if (p) std::memcpy(p, meta.data(), static_cast<size_t>(bd.size));
        }
    }

    // ---- Upload edge buffer ---------------------------------------------
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(edges.size()) * sizeof(ShardEdge);
        bd.stride     = sizeof(ShardEdge);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        if (m_gfx->CreateBuffer(bd, m_edgeBuffer))
        {
            void* p = m_gfx->MapBuffer(m_edgeBuffer);
            if (p) std::memcpy(p, edges.data(), static_cast<size_t>(bd.size));
        }
    }

    // ---- Shard buffer (DEFAULT, GPU-managed) ----------------------------
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(m_shardCount) * sizeof(ShardGPU);
        bd.stride     = sizeof(ShardGPU);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        if (!m_gfx->CreateBuffer(bd, m_shardBuffer))
            LOG_ERROR("GlassShatterPass: shard buffer creation failed");
    }
}

// ============================================================================
// SetCrackColor / Trigger
// ============================================================================
void GlassShatterPass::SetCrackColor(float r, float g, float b)
{
    m_crackColor = { r, g, b };
}

void GlassShatterPass::Trigger(float impactU, float impactV)
{
    m_impactUV          = { std::clamp(impactU, 0.0f, 1.0f),
                            std::clamp(impactV, 0.0f, 1.0f) };
    m_active            = true;
    m_pendingInit       = true;
    m_timeSinceTrigger  = 0.0f;
}

// ============================================================================
// Source texture lazy create — match Tonemap output (R8G8B8A8_UNORM)
// ============================================================================
void GlassShatterPass::EnsureSourceTexture(uint32_t w, uint32_t h)
{
    if (m_shatterSource.IsValid() && w == m_sourceW && h == m_sourceH) return;

    if (m_shatterSource.IsValid()) m_gfx->DestroyTexture(m_shatterSource);

    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.format     = RHI::Format::R8G8B8A8_UNORM;   // matches ToneMapPass output
    td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::COPY_DST;
    if (!m_gfx->CreateTexture(td, m_shatterSource))
    {
        LOG_ERROR("GlassShatterPass: source texture create failed (%ux%u)", w, h);
        m_sourceW = m_sourceH = 0;
        return;
    }
    m_shatterSourceState = RHI::ResourceState::COPY_DST;
    m_sourceW = w;
    m_sourceH = h;
}

// ============================================================================
// CB upload
// ============================================================================
void GlassShatterPass::UploadCB(uint32_t w, uint32_t h)
{
    if (!m_paramsCBMapped) return;
    ShatterCB c{};
    c.TimeSinceTrigger     = m_timeSinceTrigger;
    c.DeltaTime            = 0.0f;  // overwritten per-Execute
    c.Duration             = m_duration;
    c.ShardCount           = m_shardCount;
    c.ImpactPointUV        = m_impactUV;
    c.ImpactRadialStrength = m_impactStrength;
    c.GravityNDC           = m_gravityNDC;
    c.AirDrag              = m_airDrag;
    c.AngularDamping       = m_angularDamping;
    c.CrackWidth           = m_crackWidth;
    c.HoldDuration         = m_holdDuration;
    c.CrackColor           = m_crackColor;
    c.Width                = w;
    c.Height               = h;
    std::memcpy(m_paramsCBMapped, &c, sizeof(c));
}

// ============================================================================
// Execute — runs on the same compute CL as Post-Process Stack, after Tonemap
// ============================================================================
void GlassShatterPass::Execute(RHI::CommandList cl,
                               const RHI::Texture* tonemapOutput,
                               RHI::ResourceState tonemapEntryExitState,
                               uint32_t vpW, uint32_t vpH,
                               float deltaTime)
{
    if (!m_active || !tonemapOutput || vpW == 0 || vpH == 0) return;
    if (!m_initPSO.IsValid() || !m_simulatePSO.IsValid() || !m_compositePSO.IsValid()) return;
    if (!m_shardBuffer.IsValid() || !m_metaBuffer.IsValid() || !m_edgeBuffer.IsValid()) return;

    // Auto-deactivate when duration elapsed.
    m_timeSinceTrigger += deltaTime;
    if (m_timeSinceTrigger >= m_duration)
    {
        m_active            = false;
        m_pendingInit       = false;
        m_timeSinceTrigger  = 0.0f;
        return;
    }

    EnsureSourceTexture(vpW, vpH);
    if (!m_shatterSource.IsValid()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Defensive same-state guard — DX12 rejects barriers where before==after.
    auto SafeImage = [&](const RHI::Texture* tex,
                         RHI::ResourceState from,
                         RHI::ResourceState to)
    {
        if (from == to || !tex) return;
        gfx.PushBarrier(RHI::GPUBarrier::Image(tex, from, to), cl);
    };

    // ---- Trigger frame: capture Tonemap output → ShatterSource ----------
    if (m_pendingInit)
    {
        SafeImage(tonemapOutput, tonemapEntryExitState, RHI::ResourceState::COPY_SRC);
        SafeImage(&m_shatterSource, m_shatterSourceState, RHI::ResourceState::COPY_DST);
        m_shatterSourceState = RHI::ResourceState::COPY_DST;

        m_gfx->CopyTextureSubresource(*tonemapOutput, 0, 0,
                                      m_shatterSource,  0, 0, cl);

        SafeImage(&m_shatterSource, RHI::ResourceState::COPY_DST,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        m_shatterSourceState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

        SafeImage(tonemapOutput, RHI::ResourceState::COPY_SRC,
                  RHI::ResourceState::UNORDERED_ACCESS);
    }
    else
    {
        SafeImage(tonemapOutput, tonemapEntryExitState,
                  RHI::ResourceState::UNORDERED_ACCESS);
    }

    UploadCB(vpW, vpH);
    // DeltaTime needs separate write — patch directly.
    if (m_paramsCBMapped)
    {
        ShatterCB* p = reinterpret_cast<ShatterCB*>(m_paramsCBMapped);
        p->DeltaTime = deltaTime;
    }

    // ---- Init dispatch (trigger frame only) -----------------------------
    if (m_pendingInit)
    {
        gfx.BindComputePipelineState(m_initPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_paramsCB, cl);
        gfx.SetComputeDescriptorTable(kSRV0, gfx.GetBufferSRVGpuHandle(m_metaBuffer), cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetBufferUAVGpuHandle(m_shardBuffer), cl);
        gfx.DispatchCompute((m_shardCount + 63) / 64, 1, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_shardBuffer), cl);
        m_pendingInit = false;
    }

    // ---- Simulate dispatch ----------------------------------------------
    gfx.BindComputePipelineState(m_simulatePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramsCB, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetBufferUAVGpuHandle(m_shardBuffer), cl);
    gfx.DispatchCompute((m_shardCount + 63) / 64, 1, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_shardBuffer), cl);

    // ---- Composite dispatch — reads Edges (SRV from UPLOAD heap, GENERIC_READ),
    // ShatterSource (SR_COMPUTE), Shards (UAV — read-only RW), writes Tonemap UAV.
    gfx.BindComputePipelineState(m_compositePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramsCB, cl);
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetBufferSRVGpuHandle(m_edgeBuffer),  cl);
    gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_shatterSource), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(*tonemapOutput), cl);
    gfx.SetComputeDescriptorTable(kUAV1, gfx.GetBufferUAVGpuHandle(m_shardBuffer), cl);
    gfx.DispatchCompute((vpW + 7) / 8, (vpH + 7) / 8, 1, cl);

    // Restore Tonemap output to its entry state so the caller's state-tracking
    // (ToneMapPass::m_finalOutputState) stays correct.
    SafeImage(tonemapOutput, RHI::ResourceState::UNORDERED_ACCESS, tonemapEntryExitState);
}
