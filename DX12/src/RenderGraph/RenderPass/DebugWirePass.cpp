#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>

using namespace DirectX;

void DebugWirePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::DebugWire_VS, RHI::ShaderStage::VS, "DebugWire.vs.hlsl");
    m_shaderLib.Register(ShaderID::DebugWire_PS, RHI::ShaderStage::PS, "DebugWire.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    // UPLOAD heap vertex buffer (CPU-writable each frame).
    RHI::GPUBufferDesc bd{};
    bd.size       = static_cast<uint64_t>(kMaxVertices) * sizeof(LineVertex);
    bd.stride     = 0; // raw buffer
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
    if (gfx.CreateBuffer(bd, m_vertexBuffer))
        m_vertexMapped = gfx.MapBuffer(m_vertexBuffer);

    if (m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_SUCCESS("DebugWirePass: initialized");
    else
        LOG_ERROR("DebugWirePass: PSO creation failed");
}

PSODesc DebugWirePass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::DebugWire_VS;
    desc.psID        = ShaderID::DebugWire_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::LINELIST;

    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = false;

    // Depth read-only (draw on top of scene but behind near objects).
    desc.dss.depth_enable     = false;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;

    // Alpha blend for semi-transparent lines.
    auto& rt = desc.bs.render_target[0];
    rt.blend_enable          = true;
    rt.src_blend             = RHI::Blend::SRC_ALPHA;
    rt.dest_blend            = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op              = RHI::BlendOp::ADD;
    rt.src_blend_alpha       = RHI::Blend::ONE;
    rt.dest_blend_alpha      = RHI::Blend::ZERO;
    rt.blend_op_alpha        = RHI::BlendOp::ADD;
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT; // HDR target
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;

    return desc;
}

void DebugWirePass::AddLine(const XMFLOAT3& a, const XMFLOAT3& b, uint32_t color)
{
    if (m_vertexCount + 2 > kMaxVertices) return;
    auto* verts = static_cast<LineVertex*>(m_vertexMapped);
    if (!verts) return;

    verts[m_vertexCount++] = { a.x, a.y, a.z, color };
    verts[m_vertexCount++] = { b.x, b.y, b.z, color };
}

void DebugWirePass::AddAABB(const XMFLOAT3& mn, const XMFLOAT3& mx, uint32_t color)
{
    // 8 corners
    XMFLOAT3 c[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z},
        {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z},
        {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z},
        {mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z},
    };
    // 12 edges
    AddLine(c[0], c[1], color); AddLine(c[2], c[3], color);
    AddLine(c[4], c[5], color); AddLine(c[6], c[7], color);
    AddLine(c[0], c[2], color); AddLine(c[1], c[3], color);
    AddLine(c[4], c[6], color); AddLine(c[5], c[7], color);
    AddLine(c[0], c[4], color); AddLine(c[1], c[5], color);
    AddLine(c[2], c[6], color); AddLine(c[3], c[7], color);
}

void DebugWirePass::AddCross(const XMFLOAT3& pos, float size, uint32_t color)
{
    // 3 axis-aligned line segments through the centre — total 6 vertices.
    AddLine({ pos.x - size, pos.y, pos.z }, { pos.x + size, pos.y, pos.z }, color);
    AddLine({ pos.x, pos.y - size, pos.z }, { pos.x, pos.y + size, pos.z }, color);
    AddLine({ pos.x, pos.y, pos.z - size }, { pos.x, pos.y, pos.z + size }, color);
}

void DebugWirePass::AddFrustum(const XMFLOAT3 corners[8], uint32_t color)
{
    // Near plane edges (corners 0-3)
    AddLine(corners[0], corners[1], color);
    AddLine(corners[1], corners[3], color);
    AddLine(corners[3], corners[2], color);
    AddLine(corners[2], corners[0], color);
    // Far plane edges (corners 4-7)
    AddLine(corners[4], corners[5], color);
    AddLine(corners[5], corners[7], color);
    AddLine(corners[7], corners[6], color);
    AddLine(corners[6], corners[4], color);
    // Connecting edges
    AddLine(corners[0], corners[4], color);
    AddLine(corners[1], corners[5], color);
    AddLine(corners[2], corners[6], color);
    AddLine(corners[3], corners[7], color);
}

void DebugWirePass::AddCapsule(const XMFLOAT3& a, const XMFLOAT3& b,
                               float radius, uint32_t color, int segments)
{
    // Axis from A to B.
    XMVECTOR vA   = XMLoadFloat3(&a);
    XMVECTOR vB   = XMLoadFloat3(&b);
    XMVECTOR axis = XMVectorSubtract(vB, vA);
    float    len  = XMVectorGetX(XMVector3Length(axis));
    if (len < 1e-6f) return;
    axis = XMVectorScale(axis, 1.f / len);

    // Build orthonormal basis (axis, u, v).
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    if (std::abs(XMVectorGetX(XMVector3Dot(axis, up))) > 0.99f)
        up = XMVectorSet(1, 0, 0, 0);
    XMVECTOR u = XMVector3Normalize(XMVector3Cross(axis, up));
    XMVECTOR v = XMVector3Cross(axis, u);

    const float step = DirectX::XM_2PI / static_cast<float>(segments);

    // Circle at A
    for (int i = 0; i < segments; ++i)
    {
        float a0 = step * i, a1 = step * (i + 1);
        XMVECTOR p0 = XMVectorAdd(vA, XMVectorAdd(XMVectorScale(u, cosf(a0) * radius), XMVectorScale(v, sinf(a0) * radius)));
        XMVECTOR p1 = XMVectorAdd(vA, XMVectorAdd(XMVectorScale(u, cosf(a1) * radius), XMVectorScale(v, sinf(a1) * radius)));
        XMFLOAT3 f0, f1; XMStoreFloat3(&f0, p0); XMStoreFloat3(&f1, p1);
        AddLine(f0, f1, color);
    }
    // Circle at B
    for (int i = 0; i < segments; ++i)
    {
        float a0 = step * i, a1 = step * (i + 1);
        XMVECTOR p0 = XMVectorAdd(vB, XMVectorAdd(XMVectorScale(u, cosf(a0) * radius), XMVectorScale(v, sinf(a0) * radius)));
        XMVECTOR p1 = XMVectorAdd(vB, XMVectorAdd(XMVectorScale(u, cosf(a1) * radius), XMVectorScale(v, sinf(a1) * radius)));
        XMFLOAT3 f0, f1; XMStoreFloat3(&f0, p0); XMStoreFloat3(&f1, p1);
        AddLine(f0, f1, color);
    }
    // Connecting lines (4 rails along the cylinder)
    for (int i = 0; i < 4; ++i)
    {
        float ang = step * (i * segments / 4);
        XMVECTOR off = XMVectorAdd(XMVectorScale(u, cosf(ang) * radius), XMVectorScale(v, sinf(ang) * radius));
        XMFLOAT3 fa, fb;
        XMStoreFloat3(&fa, XMVectorAdd(vA, off));
        XMStoreFloat3(&fb, XMVectorAdd(vB, off));
        AddLine(fa, fb, color);
    }
}

void DebugWirePass::Execute(RHI::CommandList cl, const RHI::Texture* depthTex,
                            const RHI::GPUBuffer& perViewCB)
{
    if (!enabled || m_vertexCount == 0) return;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc());
    if (!pso || !pso->IsValid()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Bind HDR + depth (read-only)
    gfx.SetRenderTargetToHdrWithDepth(depthTex, cl);

    gfx.BindDescriptorHeaps(cl);
    gfx.BindPipelineState(*pso, cl);

    // Bind PerView CB at b1 space0. BindConstantBuffer's `slot` is 0-based
    // relative to kCBVSlotBase (b1=0, b2=1, ...), so slot=0 maps to b1.
    // Passing 1 here used to bind to b2 — the shader's b1 was left undefined,
    // viewProj read as garbage, and every vertex transformed off-screen. The
    // wireframe was technically drawn every frame but never visible until a
    // prior frame's stale b1 descriptor happened to contain a valid matrix
    // (picking click nudged that state).
    gfx.BindConstantBuffer(perViewCB, 0, cl);

    // Bind line vertex buffer at t2 space0 (root param 10 = descriptor table).
    uint64_t vbSrv = gfx.GetBufferSRVGpuHandle(m_vertexBuffer);
    if (vbSrv)
        gfx.BindDescriptorTableGpuHandle(10, vbSrv, cl);

    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::LINELIST, cl);

    gfx.DrawInstanced(m_vertexCount, 1, 0, 0, cl);
}
