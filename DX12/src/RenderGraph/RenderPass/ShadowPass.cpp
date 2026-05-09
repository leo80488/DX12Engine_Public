#include "RenderGraph/RenderPass/ShadowPass.h"
#include "ECS/Components.h"         // ShadowCullMode enum
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/ShadowSystem.h"
#include "RenderGraph/RenderContext.h"
#include "RenderGraph/RenderPass/TerrainPass.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cstring>

using namespace DirectX;

// Root slot for per-cascade shadow view-projection CB (same as PerViewCB in GBuffer pass).
static constexpr uint32_t kShadowPerViewSlot  = 0;   // CB slot 0 → b1 space0
static constexpr uint32_t kInstanceBufSlot    = 8;
static constexpr uint32_t kMeshDescSlot       = 9;
static constexpr uint32_t kBindlessSlot       = 14;
// Terrain shadow rendering root slots.
static constexpr uint32_t kTerrainHeightmapRootSlot = 10;   // t2 space0 (per-draw SRV table, ALL vis)
static constexpr uint32_t kTerrainParamsCBSlot      = 1;    // b2 space0 (CB-relative slot 1)
static constexpr uint32_t kTerrainSamplerSlot       = 0;    // s0 space0

// ---------------------------------------------------------------------------
ShadowPass::ShadowPass() = default;

// ---------------------------------------------------------------------------
ShadowPass::~ShadowPass()
{
    if (m_gfxPtr)
    {
        for (int i = 0; i < kCascadeCount; ++i)
            if (m_cascadeCBMapped[i])
                m_gfxPtr->UnmapBuffer(m_cascadeCBs[i]);
        if (m_shadowArray.IsValid())
            m_gfxPtr->DestroyTexture(m_shadowArray);
    }
}

// ---------------------------------------------------------------------------
void ShadowPass::Setup(RG::RenderGraphBuilder& /*b*/)
{
    // Shadow pass creates its own depth texture outside the RenderGraph system.
    // No graph-managed textures are read or written.
}

// ---------------------------------------------------------------------------
void ShadowPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
    PermutationKey basePerm;
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm, RHI::CullMode::BACK)))
        LOG_ERROR("ShadowPass: PSO rebuild after shader reload failed");
    BuildTerrainShadowPSO(gfx);
}

// ---------------------------------------------------------------------------
// Terrain shadow PSO — Terrain_Shadow_MS + null PS, depth-only RT, same
// reversed-Z + depth-bias as the regular shadow PSOs so cascade rendering
// stays acne-free on terrain surfaces.
bool ShadowPass::BuildTerrainShadowPSO(IGraphicsDevice& gfx)
{
    const RHI::Shader* as = m_shaderLib.GetShader(ShaderID::Terrain_Shadow_AS);
    const RHI::Shader* ms = m_shaderLib.GetShader(ShaderID::Terrain_Shadow_MS);
    if (!as || !as->IsValid())
    {
        LOG_ERROR("ShadowPass: Terrain_Shadow_AS not found — terrain will not cast shadows");
        return false;
    }
    if (!ms || !ms->IsValid())
    {
        LOG_ERROR("ShadowPass: Terrain_Shadow_MS not found — terrain will not cast shadows");
        return false;
    }

    // Bias values must match the engine's reversed-Z convention (see
    // ShadowPass::BuildPSODesc — BACK-cull case): depth values increase
    // toward the light, so negative depth_bias pushes the caster AWAY
    // from the receiver. With the previously-positive +1024 / +2.0 the
    // caster ended up CLOSER to the receiver every frame → solid acne
    // across the entire terrain.
    //
    // depth_clip_enable=false matches the engine pattern; clamping (vs
    // clipping) at the near/far planes is the standard for shadow casting.
    RHI::RasterizerState rs{};
    rs.fill_mode               = RHI::FillMode::SOLID;
    rs.cull_mode               = RHI::CullMode::BACK;
    rs.front_counter_clockwise = false;
    rs.depth_clip_enable       = false;
    rs.depth_bias              = -50;
    rs.slope_scaled_depth_bias = -1.5f;
    rs.depth_bias_clamp        = 0.0f;

    RHI::DepthStencilState dss{};
    dss.depth_enable     = true;
    dss.depth_write_mask = RHI::DepthWriteMask::ALL;
    dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;   // reversed Z
    dss.stencil_enable   = false;

    RHI::BlendState bs{};   // depth-only — no colour writes happen anyway

    RHI::PipelineStateDesc pd{};
    pd.as          = as;
    pd.ms          = ms;
    pd.ps          = nullptr;     // depth-only
    pd.rs          = &rs;
    pd.dss         = &dss;
    pd.bs          = &bs;
    pd.il          = nullptr;
    pd.pt          = RHI::PrimitiveTopology::TRIANGLELIST;     // ignored for MS PSO
    pd.rtv_count   = 0;
    pd.dsv_format  = RHI::Format::D32_FLOAT;                   // matches m_shadowArray
    pd.sample_count = 1;

    if (!gfx.CreatePipelineState(pd, m_terrainShadowPSO))
    {
        LOG_ERROR("ShadowPass: terrain shadow PSO creation failed");
        return false;
    }
    LOG_INFO("ShadowPass: terrain shadow PSO ready");
    return true;
}

// ---------------------------------------------------------------------------
// Per-cascade terrain dispatch. Binds the cascade's ShadowPerViewCB at b1
// (the regular DrawPacket loop above may have skipped this if no opaque
// casters were present), plus the terrain-specific resources, then
// DispatchMesh. The PSO uses the same default root signature as the rest
// of the pass so existing per-CL bindings (descriptor heaps) persist.
void ShadowPass::RenderTerrainShadow(RHI::CommandList cl, int cascadeIdx)
{
    if (!m_terrainPass || !m_terrainShadowPSO.IsValid())     return;
    if (cascadeIdx < 0 || cascadeIdx >= kCascadeCount)       return;
    if (!m_cascadeCBs[cascadeIdx].IsValid())                 return;
    if (!m_terrainParamsCB || !m_terrainParamsCB->IsValid()) return;

    const auto& tile = m_terrainPass->GetTileBindings();
    if (tile.dispatchAsGroupCount == 0 || tile.heightmapSRV == 0) return;

    cl.SetPipelineState(m_terrainShadowPSO);
    cl.BindDescriptorHeaps();

    // b1 cascade ShadowPerViewCB (idempotent re-bind in case the regular
    // draw loop never touched this slot for this cascade).
    cl.GetDevice().BindConstantBuffer(m_cascadeCBs[cascadeIdx], kShadowPerViewSlot, cl);

    // b2 TerrainCB. ShadowPass is standalone (not in m_graph), so the
    // graph-published "TerrainParams" lookup via BindCBByName fails here.
    // Bind directly through the device — same pattern as the cascade CB
    // above. The buffer pointer is wired by Renderer once at startup.
    cl.GetDevice().BindConstantBuffer(*m_terrainParamsCB, kTerrainParamsCBSlot, cl);

    // t2 heightmap SRV. Same descriptor table slot as the colour pass.
    cl.BindDescriptorTableHandle(kTerrainHeightmapRootSlot, tile.heightmapSRV);

    if (m_terrainShadowSamplerIdx >= 0)
        cl.BindSampler(kTerrainSamplerSlot, m_terrainShadowSamplerIdx);

    cl.DispatchMesh(tile.dispatchAsGroupCount, 1, 1);
}

void ShadowPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    // ---- Compile shadow shaders / PSO --------------------------------------
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Shadow_VS, RHI::ShaderStage::VS, "Shadow.vs.hlsl");
    m_shaderLib.Register(ShaderID::Shadow_PS, RHI::ShaderStage::PS, "Shadow.ps.hlsl");
    m_shaderLib.Register(ShaderID::Terrain_Shadow_AS, RHI::ShaderStage::AS, "Terrain.shadow.as.hlsl");
    m_shaderLib.Register(ShaderID::Terrain_Shadow_MS, RHI::ShaderStage::MS, "Terrain.shadow.ms.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    PermutationKey basePerm;
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm, RHI::CullMode::BACK)))
        LOG_ERROR("ShadowPass: base PSO creation failed");
    else
        LOG_INFO("ShadowPass: base PSO ready");

    // ---- Terrain shadow PSO + sampler --------------------------------------
    BuildTerrainShadowPSO(gfx);

    {
        RHI::SamplerDesc sd{};
        sd.filter         = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u      = RHI::TextureAddressMode::CLAMP;
        sd.address_v      = RHI::TextureAddressMode::CLAMP;
        sd.address_w      = RHI::TextureAddressMode::CLAMP;
        sd.max_anisotropy = 1;
        if (!gfx.CreateSampler(sd, m_terrainShadowSamplerIdx))
            LOG_ERROR("ShadowPass: failed to create terrain shadow sampler");
    }

    // Linear-wrap sampler for alpha-test shadow PS (matches GBuffer's g_LinearWrap).
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::WRAP;
        sd.address_v = RHI::TextureAddressMode::WRAP;
        sd.address_w = RHI::TextureAddressMode::WRAP;
        if (!gfx.CreateSampler(sd, m_alphaSamplerIdx))
            LOG_ERROR("ShadowPass: failed to create alpha-test wrap sampler");
    }

    // ---- Create shadow Texture2DArray (kShadowMapSize × kShadowMapSize × kCascadeCount) ---
    // Using a single array resource avoids 3 separate SRV bindings and allows the
    // shader to sample via float3(uv, sliceIndex) with no dynamic indexing overhead.
    {
        RHI::TextureDesc td;
        td.width      = kShadowMapSize;
        td.height     = kShadowMapSize;
        td.format     = RHI::Format::D32_FLOAT;
        td.bind_flags = RHI::BindFlag::DEPTH_STENCIL | RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.mip_levels = 1;
        td.array_size = kCascadeCount;    // 3 array slices — one per cascade
        td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0);  // reversed Z
        td.debug_name = "ShadowPass.CascadeArray";

        if (!gfx.CreateTexture(td, m_shadowArray))
        {
            LOG_ERROR("ShadowPass: shadow Texture2DArray creation failed");
            return;
        }
    }

    // Per-slice DSVs are auto-created by IGraphicsDevice::CreateTexture when
    // array_size > 1 + DEPTH_STENCIL bind flag — no raw DX12 needed here.

    // ---- Create per-cascade shadow VP constant buffers (UPLOAD) ------------
    // Layout (must match Shadow.vs.hlsl + Terrain.shadow.{as,ms}.hlsl):
    //   float4x4 shadowViewProj;           //  64 B  (transposed for HLSL row-vector)
    //   float4   shadowFrustumPlanes[6];   //  96 B  (inward-normal world-space planes)
    // Total 160 B, padded to 256 B by CB alignment. Terrain.shadow.as.hlsl
    // reads the planes for per-cascade frustum cull; the VS / MS only declare
    // the matrix and ignore the tail bytes.
    {
        RHI::GPUBufferDesc desc;
        desc.size       = (sizeof(XMFLOAT4X4) + 6u * sizeof(XMFLOAT4) + 255u) & ~255u;  // 256-byte aligned CB
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;

        for (int i = 0; i < kCascadeCount; ++i)
        {
            if (!gfx.CreateBuffer(desc, m_cascadeCBs[i]))
            {
                LOG_ERROR("ShadowPass: cascade %d CB creation failed", i);
                continue;
            }
            m_cascadeCBMapped[i] = gfx.MapBuffer(m_cascadeCBs[i]);
            if (!m_cascadeCBMapped[i])
                LOG_ERROR("ShadowPass: cascade %d CB map failed", i);
        }
    }

    // ---- Create ExecuteIndirect arg buffer (UPLOAD, persistent map) ----------
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(kMaxIndirectCommands) * sizeof(IndirectDrawCommand);
        desc.stride     = sizeof(IndirectDrawCommand);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::NONE;
        if (gfx.CreateBuffer(desc, m_indirectArgBuffer))
            m_indirectArgMapped = gfx.MapBuffer(m_indirectArgBuffer);
    }

    LOG_SUCCESS("ShadowPass: initialized — Texture2DArray %ux%ux%d, indirect=%s",
        kShadowMapSize, kShadowMapSize, kCascadeCount,
        m_indirectArgMapped ? "YES" : "NO");
}

// ---------------------------------------------------------------------------
uint64_t ShadowPass::GetShadowArrayGpuHandle() const
{
    if (!m_gfxPtr || !m_shadowArray.IsValid()) return 0;
    return m_gfxPtr->GetTextureSRVGpuHandle(m_shadowArray);
}

// ---------------------------------------------------------------------------
PSODesc ShadowPass::BuildPSODesc(PermutationKey perm, RHI::CullMode cullMode) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::Shadow_VS;
    desc.psID        = ShaderID::Shadow_PS;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;  // PVF

    // Cull mode is now caller-supplied (per-material ShadowCullMode).
    //   BACK  — default for opaque materials (bias fights self-shadow acne)
    //   FRONT — closed-mesh characters (only back faces hit shadow map, so
    //           self-shadow acne is physically impossible — bias NOT needed
    //           and actually HARMFUL: front-cull's back faces are grazing
    //           w.r.t. the light, slope |dz/dx| explodes, and slope-scaled
    //           bias then pushes stored depth clear past the receiver —
    //           shadow silently vanishes on other surfaces)
    //   NONE  — single-sided geometry (hair cards, cloth, foliage cards)
    //           needs a middle ground — both sides rasterize, acne still
    //           possible.
    //
    // Alpha-test is a second axis: forces NONE cull regardless of material.
    //
    // Caster-side slope-scaled bias is the FIRST line of defence; the receiver
    // side now adds normal-offset and receiver-plane depth bias (shadow.hlsli),
    // so these slope-scaled values can stay conservative — they only need to
    // catch the residual that survives the receiver biases.
    desc.rs.cull_mode               = cullMode;
    desc.rs.depth_clip_enable       = false;
    desc.rs.depth_bias_clamp        = 0.0f;

    switch (cullMode)
    {
    case RHI::CullMode::FRONT:
        // Trust the cull. Tiny bias just as precision safety.
        desc.rs.depth_bias              = -2;
        desc.rs.slope_scaled_depth_bias = -0.25f;
        break;
    case RHI::CullMode::NONE:
        // Alpha-test cards / single-sided geometry. Both sides rasterize,
        // so self-shadow is possible — but the surfaces are usually thin
        // so full BACK-cull bias is overkill and produces visible gaps.
        desc.rs.depth_bias              = -30;
        desc.rs.slope_scaled_depth_bias = -1.0f;
        break;
    default: // BACK
        desc.rs.depth_bias              = -50;
        desc.rs.slope_scaled_depth_bias = -1.5f;
        break;
    }

    // Depth test and write, no color output.
    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ALL;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.rtvCount  = 0;                           // depth-only
    desc.dsvFormat = RHI::Format::D32_FLOAT;
    return desc;
}

// ---------------------------------------------------------------------------
// Pack the per-cascade ShadowPerViewCB:
//   [   0 ..  63] transposed VP (HLSL row-vector convention)
//   [  64 .. 159] 6 inward-normal world-space frustum planes
// extracted from the UN-transposed cascade VP via Gribb-Hartmann. Terrain.
// shadow.as.hlsl reads the plane block for per-cascade culling.
void ShadowPass::UploadCascadeCBs()
{
    if (!m_sys) return;
    const DirectX::XMFLOAT4X4* src = m_sys->CascadeMatricesForPass();
    for (int i = 0; i < kCascadeCount; ++i)
    {
        if (!m_cascadeCBMapped[i]) continue;

        // ---- Matrix ----
        XMMATRIX mat = XMLoadFloat4x4(&src[i]);
        XMFLOAT4X4 transposed;
        XMStoreFloat4x4(&transposed, XMMatrixTranspose(mat));

        uint8_t* dst = static_cast<uint8_t*>(m_cascadeCBMapped[i]);
        std::memcpy(dst, &transposed, sizeof(transposed));

        // ---- Frustum planes (from the untransposed VP, Gribb-Hartmann) ----
        // Same convention as RenderTypes.h ExtractFrustumPlanes — inward-normal
        // planes, dot(plane.xyz, P) + plane.w ≥ 0 ⇒ inside.
        const auto& m = src[i].m;
        auto makePlane = [](float a, float b, float c, float d, float* out) {
            const float invLen = 1.0f / std::sqrt(a*a + b*b + c*c);
            out[0] = a * invLen;
            out[1] = b * invLen;
            out[2] = c * invLen;
            out[3] = d * invLen;
        };
        float planes[6][4];
        // Left:   col[3] + col[0]
        makePlane(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0], planes[0]);
        // Right:  col[3] - col[0]
        makePlane(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0], planes[1]);
        // Bottom: col[3] + col[1]
        makePlane(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1], planes[2]);
        // Top:    col[3] - col[1]
        makePlane(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1], planes[3]);
        // Near:   col[2]
        makePlane(m[0][2],           m[1][2],           m[2][2],           m[3][2],           planes[4]);
        // Far:    col[3] - col[2]
        makePlane(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2], planes[5]);

        std::memcpy(dst + sizeof(transposed), planes, sizeof(planes));
    }
}

// ---------------------------------------------------------------------------
RHI::CommandList ShadowPass::Execute(RHI::CommandList cl)
{
    DrawList opaqueDraws      = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    DrawList shadowDraws      = cl.GetContext().GetDrawList(DrawFilter::Shadow);
    // Transparents only contribute when alpha-tested (foliage with BlendMode::Alpha + alphaRef).
    DrawList transparentDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);
    if (opaqueDraws.empty() && shadowDraws.empty() && transparentDraws.empty()) return cl;

    if (!m_shadowArray.IsValid()) return cl;

    UploadCascadeCBs();

    IGraphicsDevice& gfx = *m_gfxPtr;

    if (!m_firstExecution)
        cl.PushBarrier(RHI::GPUBarrier::Image(
            &m_shadowArray,
            RHI::ResourceState::DEPTH_READ_SRV,
            RHI::ResourceState::DEPTHSTENCIL));

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const bool useIndirect = (m_indirectArgMapped != nullptr);

    // ---- 4-group classification --------------------------------------------
    // Alpha-test is handled as a single bucket (shader permutation differs,
    // cull is forced NONE regardless of material setting — alpha-test cards
    // are inherently single-sided).
    //
    // For non-alpha-test draws we honour the per-material ShadowCullMode:
    //   Default (0) → BACK   : bias-based anti-acne, legacy path
    //   Front   (1) → FRONT  : closed-mesh character short-circuit (no acne)
    //   None    (2) → NONE   : cloth / single-sided props that need 2-sided
    enum DrawGroup {
        GRP_OPAQUE_BACK  = 0,
        GRP_OPAQUE_FRONT = 1,
        GRP_OPAQUE_NONE  = 2,
        GRP_ALPHATEST    = 3,
        GRP_COUNT        = 4,
        GRP_SKIP         = -1,   // material opted out of casting shadow
    };

    auto classify = [](const DrawPacket& dp) -> int {
        if (!dp.castShadow) return GRP_SKIP;
        // Transparent (Alpha/Premul/Additive/Multiply) packets reach this pass only
        // because foliage opts in via alphaRef. Skip the rest — additive sparks /
        // multiply decals would render solid silhouettes otherwise.
        if (dp.filter == DrawFilter::Transparent && !dp.permutation.Has(PermutationKey::ALPHA_TEST))
            return GRP_SKIP;
        if (dp.permutation.Has(PermutationKey::ALPHA_TEST))
            return GRP_ALPHATEST;
        switch (static_cast<ShadowCullMode>(dp.shadowCullMode))
        {
        case ShadowCullMode::Front: return GRP_OPAQUE_FRONT;
        case ShadowCullMode::None:  return GRP_OPAQUE_NONE;
        default:                    return GRP_OPAQUE_BACK;
        }
    };

    uint32_t groupOffsets[GRP_COUNT] = {};   // byte offset into indirect buffer
    uint32_t groupCounts [GRP_COUNT] = {};   // command count

    if (useIndirect)
    {
        auto* args = static_cast<IndirectDrawCommand*>(m_indirectArgMapped);

        // Pass 1: count per group (SKIP bucket ignored).
        auto countList = [&](DrawList list) {
            for (const DrawPacket& dp : list)
            {
                const int g = classify(dp);
                if (g < 0 || g >= GRP_COUNT) continue;
                ++groupCounts[g];
            }
        };
        countList(opaqueDraws); countList(shadowDraws); countList(transparentDraws);

        // Prefix-sum → byte offsets
        uint32_t running = 0;
        for (int g = 0; g < GRP_COUNT; ++g)
        {
            groupOffsets[g] = running * sizeof(IndirectDrawCommand);
            running += groupCounts[g];
        }
        const uint32_t totalCommands = running;
        if (totalCommands > kMaxIndirectCommands)
        {
            // Cap — scale group counts proportionally so we never ExecuteIndirect
            // past the buffer tail. (Under-dimensioning is a LOG-worthy warning
            // but the shadows still render from the clipped front of each group.)
            LOG_WARNING("ShadowPass: %u commands exceed cap %u — clipping",
                        totalCommands, kMaxIndirectCommands);
        }

        // Pass 2: write per group, using per-group write cursors.
        uint32_t cursors[GRP_COUNT];
        for (int g = 0; g < GRP_COUNT; ++g)
            cursors[g] = groupOffsets[g] / sizeof(IndirectDrawCommand);

        auto writeList = [&](DrawList list) {
            for (const DrawPacket& dp : list)
            {
                const int g = classify(dp);
                if (g < 0 || g >= GRP_COUNT) continue;   // opted-out caster
                uint32_t  c = cursors[g];
                if (c >= kMaxIndirectCommands) continue;
                IndirectDrawCommand& ic = args[c];
                ic.meshDescIdx            = dp.meshDescriptorIndex;
                ic.instanceOffset         = dp.instanceOffset;
                ic.materialIndex          = dp.materialIndex;
                ic.prevPosInfo            = 0;
                ic.vertexCountPerInstance = dp.vertexOrIndexCount;
                ic.instanceCount          = dp.instanceCount;
                ic.startVertexLocation    = 0;
                ic.startInstanceLocation  = 0;
                cursors[g] = c + 1;
            }
        };
        writeList(opaqueDraws); writeList(shadowDraws); writeList(transparentDraws);
    }

    // ---- Per-group PSO descs (base shader perm + rasterizer cull mode) ----
    struct GroupPSO { PermutationKey perm; RHI::CullMode cull; };
    GroupPSO groupPSO[GRP_COUNT];
    groupPSO[GRP_OPAQUE_BACK].cull   = RHI::CullMode::BACK;
    groupPSO[GRP_OPAQUE_FRONT].cull  = RHI::CullMode::FRONT;
    groupPSO[GRP_OPAQUE_NONE].cull   = RHI::CullMode::NONE;
    groupPSO[GRP_ALPHATEST].cull     = RHI::CullMode::NONE;
    groupPSO[GRP_ALPHATEST].perm.Set(PermutationKey::ALPHA_TEST, true);

    // ---- Render each cascade -----------------------------------------------
    for (int cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        if (!m_cascadeCBs[cascade].IsValid()) continue;

        gfx.ClearDepthStencilSlice(m_shadowArray, cascade, 0.0f, 0, cl);  // reversed Z
        gfx.SetDepthStencilSlice(m_shadowArray, cascade, cl);

        cl.SetViewport(kShadowMapSize, kShadowMapSize);
        cl.SetScissorRect(kShadowMapSize, kShadowMapSize);
        cl.SetPrimitiveTopology();

        // Cascade kFarCascadeIdx is the dedicated ultra-far terrain cascade
        // — covers ~kShadowFarCap..kFarCascadeFar in view-Z. Dynamic
        // entities (characters, props) are sub-pixel at that distance and
        // the cost of drawing every DrawPacket into a 4096² target a fourth
        // time is wasted; skip the regular draw loop entirely and let the
        // terrain mesh-shader pass below own this cascade slice.
        const bool terrainOnlyCascade = (cascade == kFarCascadeIdx);

        for (int g = 0; !terrainOnlyCascade && g < GRP_COUNT; ++g)
        {
            if (useIndirect && groupCounts[g] == 0) continue;

            const RHI::PipelineState* pso = m_psoCache.GetOrCreate(
                BuildPSODesc(groupPSO[g].perm, groupPSO[g].cull));
            if (!pso || !pso->IsValid()) continue;

            cl.SetPipelineState(*pso);
            cl.BindDescriptorHeaps();
            cl.BindBufferSRVByName(kInstanceBufSlot, "InstanceBuffer");
            cl.BindBufferSRVByName(kMeshDescSlot,    "MeshDescriptors");
            if (bindlessHandle)
                cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
            cl.GetDevice().BindConstantBuffer(m_cascadeCBs[cascade], kShadowPerViewSlot, cl);

            // ALPHA_TEST PS reads MaterialBuffer (alphaRef) + bindless g_AllTextures[]
            // (foliage albedo). Skip these binds on the opaque groups — they don't
            // touch t2 space0 / t0 space2 and the root sig allows stale bindings.
            if (g == GRP_ALPHATEST)
            {
                if (const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer("MaterialBuffer"))
                    if (matBuf->IsValid())
                        cl.BindBufferSRV(0, *matBuf);
                if (cl.gfx)
                {
                    auto& dx12 = static_cast<GraphicsDX12&>(*cl.gfx);
                    D3D12_GPU_DESCRIPTOR_HANDLE texTable = dx12.GetBindlessTextureTableHandle();
                    if (texTable.ptr)
                        cl.BindDescriptorTableHandle(27, texTable.ptr);
                }
                if (m_alphaSamplerIdx >= 0)
                    cl.BindSampler(0, m_alphaSamplerIdx);
            }

            if (useIndirect)
            {
                gfx.ExecuteIndirectDraw(m_indirectArgBuffer,
                                        groupOffsets[g],
                                        groupCounts[g],
                                        nullptr, 0, cl);
            }
            else
            {
                // Fallback: per-draw path. Use the SAME classifier so a
                // packet only emits a draw when its group is the current one.
                auto fallbackDraw = [&](DrawList list) {
                    for (const DrawPacket& dp : list) {
                        if (classify(dp) != g) continue;
                        cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
                        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
                    }
                };
                fallbackDraw(opaqueDraws);
                fallbackDraw(shadowDraws);
                fallbackDraw(transparentDraws);
            }
        }

        // Terrain shadow casting — depth-only mesh-shader pass into the
        // same cascade slice. No-op when no terrain entity exists or its
        // heightmap isn't GPU-resident yet.
        RenderTerrainShadow(cl, cascade);
    }

    cl.PushBarrier(RHI::GPUBarrier::Image(
        &m_shadowArray,
        RHI::ResourceState::DEPTHSTENCIL,
        RHI::ResourceState::DEPTH_READ_SRV));

    m_firstExecution = false;
    return cl;
}
