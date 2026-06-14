#include "RenderGraph/RenderPass/BillboardFXPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderContext.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"   // GlobalTransform
#include "Resource/ResourceManager.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace DirectX;

// Engine default root-signature slots reused by this pass (must match the
// shared root sig in GraphicsDX12.cpp — same slots WorldUIBillboardPass uses):
//   b1 space0  — PerView CB (we upload our own viewProj here)
//   t2 space0  — vertex ByteAddressBuffer (slot 10)
//   t0 space2  — bindless Texture2D[] table (slot 27)
//   s0         — linear-clamp sampler
static constexpr uint32_t kRootSlot_VertexSRV   = 10; // t2 space0
static constexpr uint32_t kRootSlot_BindlessTex = 27; // t0 space2 (engine-shared)

// ---------------------------------------------------------------------------
BillboardFXPass::BillboardFXPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

// ---------------------------------------------------------------------------
void BillboardFXPass::Setup(RG::RenderGraphBuilder& b)
{
    // Read-only depth (test, no write). Declaring WriteDepthStencil makes the
    // graph emit the SHADER_RESOURCE → DEPTHSTENCIL barrier, same as
    // TransparentPass / ParticleRenderPass which sit in the same HDR phase.
    b.WriteDepthStencil(m_depth);
    // The HDR RTV is bound manually in Execute (not a graph-managed texture).
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// ---------------------------------------------------------------------------
void BillboardFXPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::BillboardFX_VS, RHI::ShaderStage::VS, "BillboardFX.vs.hlsl");
    m_shaderLib.Register(ShaderID::BillboardFX_PS, RHI::ShaderStage::PS, "BillboardFX.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    // Triple-buffered UPLOAD vertex ring (flat-expanded, no IA index buffer —
    // the engine root sig vertex-pulls from a ByteAddressBuffer).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxVertices) * sizeof(Vertex);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_vertexBuffer[i]))
                m_vbMapped[i] = gfx.MapBuffer(m_vertexBuffer[i]);
            else
                LOG_ERROR("BillboardFXPass: VB[%u] creation failed", i);
        }
    }

    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        if (!gfx.CreateSampler(sd, m_samplerIdx))
            LOG_ERROR("BillboardFXPass: sampler creation failed");
    }

    // Warm up the common PSO permutations (additive + alpha, depth-tested).
    m_psoCache.GetOrCreate(BuildPSODesc(BillboardFXBlend::Additive, true));
    m_psoCache.GetOrCreate(BuildPSODesc(BillboardFXBlend::Alpha,    true));
    LOG_SUCCESS("BillboardFXPass: initialized");
}

// ---------------------------------------------------------------------------
PSODesc BillboardFXPass::BuildPSODesc(BillboardFXBlend blend, bool depthTest) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::BillboardFX_VS;
    desc.psID        = ShaderID::BillboardFX_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::TRIANGLELIST;

    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = true;

    // Depth test as requested, never depth write. Reversed-Z (GREATER_EQUAL) to
    // match the engine's depth buffer. dsvFormat stays bound (matching the DSV)
    // even when the test is disabled, so the PSO ↔ bound-DSV format agrees.
    desc.dss.depth_enable     = depthTest;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;

    auto& rt = desc.bs.render_target[0];
    rt.blend_enable             = true;
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    rt.blend_op                 = RHI::BlendOp::ADD;
    rt.blend_op_alpha           = RHI::BlendOp::ADD;
    rt.src_blend                = RHI::Blend::SRC_ALPHA;
    rt.src_blend_alpha          = RHI::Blend::ONE;
    if (blend == BillboardFXBlend::Additive)
    {
        rt.dest_blend       = RHI::Blend::ONE;            // SRC_ALPHA*src + dst
        rt.dest_blend_alpha = RHI::Blend::ONE;
    }
    else
    {
        rt.dest_blend       = RHI::Blend::INV_SRC_ALPHA;  // standard alpha-over
        rt.dest_blend_alpha = RHI::Blend::INV_SRC_ALPHA;
    }

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT; // HDR scene colour
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

// ---------------------------------------------------------------------------
// Pushes 6 vertices (2 triangles, CCW) for one camera-facing quad.
void BillboardFXPass::EmitQuad(std::vector<Vertex>& out,
                               const XMFLOAT3& center,
                               const XMFLOAT3& right,
                               const XMFLOAT3& up,
                               float halfW, float halfH,
                               float u0, float v0, float u1, float v1,
                               const float color[4], uint32_t texIdx)
{
    const XMVECTOR c = XMLoadFloat3(&center);
    const XMVECTOR r = XMVectorScale(XMLoadFloat3(&right), halfW);
    const XMVECTOR u = XMVectorScale(XMLoadFloat3(&up),    halfH);

    XMFLOAT3 p[4];
    XMStoreFloat3(&p[0], XMVectorSubtract(XMVectorAdd(c, u), r));      // top-L
    XMStoreFloat3(&p[1], XMVectorAdd(XMVectorAdd(c, u), r));           // top-R
    XMStoreFloat3(&p[2], XMVectorAdd(XMVectorSubtract(c, u), r));      // bot-R
    XMStoreFloat3(&p[3], XMVectorSubtract(XMVectorSubtract(c, u), r)); // bot-L

    auto push = [&](const XMFLOAT3& xyz, float uu, float vv) {
        Vertex vtx{};
        vtx.pos[0] = xyz.x; vtx.pos[1] = xyz.y; vtx.pos[2] = xyz.z;
        vtx.uv[0]  = uu;    vtx.uv[1]  = vv;
        vtx.color[0] = color[0]; vtx.color[1] = color[1];
        vtx.color[2] = color[2]; vtx.color[3] = color[3];
        vtx.texIdx = texIdx;
        out.push_back(vtx);
    };

    push(p[0], u0, v0); push(p[1], u1, v0); push(p[2], u1, v1);
    push(p[0], u0, v0); push(p[2], u1, v1); push(p[3], u0, v1);
}

// ---------------------------------------------------------------------------
void BillboardFXPass::ResolveTexture(uint32_t entity, BillboardFXComponent& b)
{
    if (!m_texSys || !m_resMgr || !m_gfx) return;

    auto [it, _inserted] = m_texCache.try_emplace(entity);
    TexCache& entry = it->second;

    if (b.texturePath != entry.path)
    {
        if (entry.handle != Resource::kInvalidTextureHandle)
            m_texSys->Release(entry.handle, *m_gfx);

        entry.path   = b.texturePath;
        entry.handle = b.texturePath.empty()
                     ? Resource::kInvalidTextureHandle
                     : m_texSys->Acquire(b.texturePath, *m_resMgr, *m_gfx);
        b.textureBindlessIdx = -1;
        b.textureGpuHandle   = 0;
    }

    if (b.textureBindlessIdx < 0
        && entry.handle != Resource::kInvalidTextureHandle
        && m_texSys->IsReady(entry.handle))
    {
        if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
        {
            b.textureBindlessIdx = static_cast<int32_t>(tex->handle_id);
            b.textureGpuHandle   = m_gfx->GetTextureSRVGpuHandle(*tex);
        }
    }
}

// ---------------------------------------------------------------------------
void BillboardFXPass::OnEntityDestroyed(uint32_t entity)
{
    auto it = m_texCache.find(entity);
    if (it == m_texCache.end()) return;
    if (m_texSys && m_gfx && it->second.handle != Resource::kInvalidTextureHandle)
        m_texSys->Release(it->second.handle, *m_gfx);
    m_texCache.erase(it);
}

// ---------------------------------------------------------------------------
void BillboardFXPass::OnWorldClear()
{
    if (m_texSys && m_gfx)
    {
        for (auto& [_entity, entry] : m_texCache)
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, *m_gfx);
    }
    m_texCache.clear();
    m_draws.clear();
}

// ---------------------------------------------------------------------------
// Advance one flipbook's playback time → current frame index.
static void AdvanceAnim(BillboardFXComponent& b, float dt)
{
    const int cols  = (b.columns > 0) ? b.columns : 1;
    const int rows  = (b.rows    > 0) ? b.rows    : 1;
    int total = (b.frameCount > 0) ? b.frameCount : (cols * rows);
    total = std::clamp(total, 1, cols * rows);

    if (total <= 1)
    {
        b.frame = 0;
        return;
    }

    if (b.playing && b.fps > 0.0f)
    {
        b.elapsed += dt;
        const int raw = static_cast<int>(b.elapsed * b.fps);
        switch (b.playback)
        {
        case BillboardFXPlayback::Once:
            b.frame = (raw >= total) ? (total - 1) : raw;
            break;
        case BillboardFXPlayback::PingPong:
        {
            const int period = 2 * total - 2;       // total > 1 here
            const int ph     = raw % period;
            b.frame = (ph < total) ? ph : (period - ph);
            break;
        }
        case BillboardFXPlayback::Loop:
        default:
            b.frame = raw % total;
            break;
        }
    }
    b.frame = std::clamp(b.frame, 0, total - 1);
}

// ---------------------------------------------------------------------------
void BillboardFXPass::BuildFrame(World& world, float dt,
                                 const XMFLOAT4X4& viewMatrix,
                                 const XMFLOAT3&   cameraPos)
{
    m_draws.clear();
    if (!enabled || !m_gfx) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);
    const uint32_t frameSlot = gfx.GetFrameIndex();
    if (!m_vbMapped[frameSlot]) return;

    auto* pool = world.GetPool<BillboardFXComponent>();
    if (!pool || pool->Size() == 0) return;

    // World-space camera axes from the row-major view matrix columns (same
    // derivation as WorldSpaceUISystem / WorldUIBillboardPass).
    const float* vm = reinterpret_cast<const float*>(&viewMatrix);
    const XMFLOAT3 camRight { vm[0], vm[4], vm[8]  };
    const XMFLOAT3 camUp    { vm[1], vm[5], vm[9]  };
    const XMFLOAT3 camFwd   { vm[2], vm[6], vm[10] };
    const XMVECTOR camPosV  = XMLoadFloat3(&cameraPos);

    // ---- Gather render records (advance anim + resolve textures) ----
    struct Rec
    {
        XMFLOAT3         pos;
        float            half;
        float            u0, v0, u1, v1;
        float            color[4];
        uint32_t         texIdx;
        BillboardFXBlend blend;
        bool             depthTest;
        BillboardFXFace  face;
        float            dist;   // squared distance to camera (for back-to-front sort)
    };
    static thread_local std::vector<Rec> recs;
    recs.clear();

    auto& ents = pool->Entities();
    auto& data = pool->Data();
    recs.reserve(data.size());

    for (size_t i = 0; i < data.size(); ++i)
    {
        BillboardFXComponent& b = data[i];
        const Entity e = ents[i];

        AdvanceAnim(b, dt);
        ResolveTexture(static_cast<uint32_t>(e), b);

        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        if (!gt) continue;   // no world pose → nothing to place

        const float alpha = std::clamp(b.tint.w * b.opacity, 0.0f, 1.0f);
        if (alpha <= 0.001f) continue;   // fully transparent (alpha) / no contribution (additive)
        if (b.size <= 0.0f)  continue;

        const int cols  = (b.columns > 0) ? b.columns : 1;
        const int rows  = (b.rows    > 0) ? b.rows    : 1;
        int total = (b.frameCount > 0) ? b.frameCount : (cols * rows);
        total = std::clamp(total, 1, cols * rows);
        const int frame = std::clamp(b.frame, 0, total - 1);
        const int col = frame % cols;
        const int row = frame / cols;

        Rec r;
        r.pos  = { gt->matrix.m[3][0], gt->matrix.m[3][1], gt->matrix.m[3][2] };
        r.half = b.size * 0.5f;
        r.u0 = static_cast<float>(col)     / static_cast<float>(cols);
        r.v0 = static_cast<float>(row)     / static_cast<float>(rows);
        r.u1 = static_cast<float>(col + 1) / static_cast<float>(cols);
        r.v1 = static_cast<float>(row + 1) / static_cast<float>(rows);
        r.color[0] = b.tint.x * b.emissive;
        r.color[1] = b.tint.y * b.emissive;
        r.color[2] = b.tint.z * b.emissive;
        r.color[3] = alpha;
        r.texIdx   = (b.textureBindlessIdx < 0)
                   ? kInvalidTexIdx
                   : static_cast<uint32_t>(b.textureBindlessIdx);
        r.blend     = b.blend;
        r.depthTest = b.depthTest;
        r.face      = b.face;

        const XMVECTOR pv = XMLoadFloat3(&r.pos);
        r.dist = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(pv, camPosV)));
        recs.push_back(r);
    }

    if (recs.empty()) return;

    // Back-to-front so alpha-blended sprites composite correctly (harmless for
    // additive, which is order-independent).
    std::sort(recs.begin(), recs.end(),
              [](const Rec& a, const Rec& b) { return a.dist > b.dist; });

    // ---- Expand to vertices, grouping consecutive same-PSO runs ----
    static thread_local std::vector<Vertex> verts;
    verts.clear();
    verts.reserve(recs.size() * 6);

    for (const Rec& r : recs)
    {
        if (verts.size() + 6 > kMaxVertices) break;   // ring full — drop the rest

        XMFLOAT3 right = camRight;
        XMFLOAT3 up    = camUp;
        if (r.face == BillboardFXFace::Cylindrical)
        {
            // Yaw to the camera but keep world-up vertical.
            const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            XMVECTOR rr = XMVector3Cross(worldUp, XMLoadFloat3(&camFwd));
            if (XMVectorGetX(XMVector3LengthSq(rr)) < 1e-6f)
                rr = XMLoadFloat3(&camRight);          // looking straight up/down
            XMStoreFloat3(&right, XMVector3Normalize(rr));
            up = { 0.0f, 1.0f, 0.0f };
        }

        const uint32_t start = static_cast<uint32_t>(verts.size());
        EmitQuad(verts, r.pos, right, up, r.half, r.half,
                 r.u0, r.v0, r.u1, r.v1, r.color, r.texIdx);

        if (!m_draws.empty()
            && m_draws.back().blend == r.blend
            && m_draws.back().depthTest == r.depthTest)
        {
            m_draws.back().count += 6;
        }
        else
        {
            m_draws.push_back({ start, 6u, r.blend, r.depthTest });
        }
    }

    if (verts.empty()) { m_draws.clear(); return; }

    std::memcpy(m_vbMapped[frameSlot], verts.data(), verts.size() * sizeof(Vertex));
}

// ---------------------------------------------------------------------------
RHI::CommandList BillboardFXPass::Execute(RHI::CommandList cl)
{
    if (!enabled || m_draws.empty()) return cl;

    // RenderGraph passes run on a WORKER command list — use the `cl.` wrapper
    // API throughout (NOT the gfx.Bind* main-CL helpers WorldUIBillboardPass
    // uses for its direct-invoke path), and bind root args AFTER the PSO so the
    // root signature is live (mirror TransparentPass).
    auto& dx12 = static_cast<GraphicsDX12&>(*cl.gfx);
    const uint32_t frameSlot = dx12.GetFrameIndex();
    if (!m_vbMapped[frameSlot]) return cl;

    // Bind the HDR RTV with the GBuffer depth (read-only test, no write).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl);

    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const uint64_t vbHandle = dx12.GetBufferSRVGpuHandle(m_vertexBuffer[frameSlot]);
    const D3D12_GPU_DESCRIPTOR_HANDLE bindlessTbl = dx12.GetBindlessTextureTableHandle();

    // SetGraphicsRootSignature (inside SetPipelineState) resets root args, so
    // re-bind globals on every PSO switch (same defensive pattern as
    // TransparentPass::bindGlobals).
    auto bindGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindCBByName(0, "PerView");                 // b1: viewProj (first matrix)
        if (m_samplerIdx >= 0) cl.BindSampler(0, m_samplerIdx);
        if (vbHandle)        cl.BindDescriptorTableHandle(kRootSlot_VertexSRV, vbHandle);
        if (bindlessTbl.ptr) cl.BindDescriptorTableHandle(kRootSlot_BindlessTex, bindlessTbl.ptr);
    };

    const RHI::PipelineState* active = nullptr;
    for (const DrawRange& d : m_draws)
    {
        const RHI::PipelineState* pso =
            m_psoCache.GetOrCreate(BuildPSODesc(d.blend, d.depthTest));
        if (!pso || !pso->IsValid()) continue;
        if (pso != active)
        {
            cl.SetPipelineState(*pso);
            active = pso;
            bindGlobals();
        }
        cl.DrawInstanced(d.count, 1, d.startVertex, 0);
    }

    return cl;
}
