#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "UI/WorldSpaceUI.h"
#include "UI/UIDrawList.h"   // Vec2 (used by Font::MeasureText return)
#include "UI/Font.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace DirectX;

// Engine default root signature slots used by this pass:
//   b1 space0  — PerView CB (we upload our own un-jittered viewProj here)
//   t2 space0  — vertex ByteAddressBuffer
//   t0 space2  — bindless Texture2D[] table (font atlas, image SRVs)
//   s0         — linear-clamp sampler
static constexpr uint32_t kRootSlot_VertexSRV     = 10; // t2 space0
static constexpr uint32_t kRootSlot_BindlessTex   = 27; // t0 space2 (engine-shared)

void WorldUIBillboardPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::WorldUI_VS, RHI::ShaderStage::VS, "WorldUI.vs.hlsl");
    m_shaderLib.Register(ShaderID::WorldUI_PS, RHI::ShaderStage::PS, "WorldUI.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    // Vertex buffer — flat-expanded (no IA index buffer; engine root sig
    // doesn't expose one).  Each frame we rebuild from scratch.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxVertices) * sizeof(WorldUIVertex);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (gfx.CreateBuffer(bd, m_vertexBuffer))
            m_vbMapped = gfx.MapBuffer(m_vertexBuffer);
        else
            LOG_ERROR("WorldUIBillboardPass: VB creation failed");
    }

    // Stub CB — engine binds PerViewCB at b1 anyway, but the root sig
    // expects some non-zero CBV bound at slot=0; reuse the same trick as
    // UIPass.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = 256;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_cb))
            m_cbMapped = gfx.MapBuffer(m_cb);
    }

    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::CLAMP;
        sd.address_v = RHI::TextureAddressMode::CLAMP;
        sd.address_w = RHI::TextureAddressMode::CLAMP;
        if (!gfx.CreateSampler(sd, m_samplerIdx))
            LOG_ERROR("WorldUIBillboardPass: sampler creation failed");
    }

    if (m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_SUCCESS("WorldUIBillboardPass: initialized");
    else
        LOG_ERROR("WorldUIBillboardPass: PSO creation failed");
}

PSODesc WorldUIBillboardPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::WorldUI_VS;
    desc.psID        = ShaderID::WorldUI_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::TRIANGLELIST;

    desc.rs.cull_mode         = RHI::CullMode::NONE;
    desc.rs.depth_clip_enable = false;

    // No depth — the pass currently runs in DepthMode::Always behaviour.
    // (DepthMode::Test will plumb scene depth as a separate variant.)
    desc.dss.depth_enable     = false;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;

    auto& rt = desc.bs.render_target[0];
    rt.blend_enable             = true;
    rt.src_blend                = RHI::Blend::SRC_ALPHA;
    rt.dest_blend               = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op                 = RHI::BlendOp::ADD;
    rt.src_blend_alpha          = RHI::Blend::ONE;
    rt.dest_blend_alpha         = RHI::Blend::INV_SRC_ALPHA;
    rt.blend_op_alpha           = RHI::BlendOp::ADD;
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R8G8B8A8_UNORM;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;
    return desc;
}

// Pivot-anchored billboard quad emitter — pushes 6 vertices (2 triangles)
// in CCW order so default cull-NONE PSO works regardless of facing.
void WorldUIBillboardPass::EmitQuad(std::vector<WorldUIVertex>& out,
                                    const XMFLOAT3& worldCenter,
                                    const XMFLOAT3& cameraRightWS,
                                    const XMFLOAT3& cameraUpWS,
                                    float halfW, float halfH,
                                    float u0, float v0, float u1, float v1,
                                    uint32_t col, uint32_t texIdx)
{
    const XMVECTOR center = XMLoadFloat3(&worldCenter);
    const XMVECTOR right  = XMVectorScale(XMLoadFloat3(&cameraRightWS), halfW);
    const XMVECTOR up     = XMVectorScale(XMLoadFloat3(&cameraUpWS),    halfH);

    XMFLOAT3 p[4];
    XMStoreFloat3(&p[0], XMVectorSubtract(XMVectorAdd(center, up), right));      // top-L
    XMStoreFloat3(&p[1], XMVectorAdd(XMVectorAdd(center, up), right));            // top-R
    XMStoreFloat3(&p[2], XMVectorAdd(XMVectorSubtract(center, up), right));       // bot-R
    XMStoreFloat3(&p[3], XMVectorSubtract(XMVectorSubtract(center, up), right));  // bot-L

    auto pushV = [&](const XMFLOAT3& xyz, float u, float v) {
        WorldUIVertex vv{};
        vv.pos[0] = xyz.x; vv.pos[1] = xyz.y; vv.pos[2] = xyz.z;
        vv.uv[0]  = u;     vv.uv[1]  = v;
        vv.col    = col;
        vv.texIdx = texIdx;
        out.push_back(vv);
    };

    pushV(p[0], u0, v0);
    pushV(p[1], u1, v0);
    pushV(p[2], u1, v1);
    pushV(p[0], u0, v0);
    pushV(p[2], u1, v1);
    pushV(p[3], u0, v1);
}

// Pack RGBA float[0..1] × alpha into uint32 0xAABBGGRR.
static uint32_t PackColor(const XMFLOAT4& c, float alpha)
{
    auto cv = [](float f) {
        return static_cast<uint8_t>(std::clamp(f, 0.f, 1.f) * 255.f);
    };
    return cv(c.x) | (uint32_t(cv(c.y)) << 8)
                   | (uint32_t(cv(c.z)) << 16)
                   | (uint32_t(cv(c.w * alpha)) << 24);
}

// Compute world-space size for one billboard given the WorldSpaceUI
// component's settings.  Out: halfW / halfH in world units.
static void WorldUIQuadHalfSize(const UI::WorldSpaceUIComponent& ws,
                                  float& halfW, float& halfH)
{
    halfW = ws.baseSize.x * 0.5f * ws.computedScale;
    halfH = ws.baseSize.y * 0.5f * ws.computedScale;
}

// Anchor the billboard quad: shift `worldCenter` so the WorldSpaceUI
// `pivot` lands on the entity world position.
//
// Vertical pivot uses WORLD UP (0,1,0), NOT camera up.  Otherwise the
// quad's world position drifts as the camera pitches — a HP bar with
// pivot=(0.5,1.0) bottom-anchored to a head would visibly slide away
// from the entity whenever the camera tilts (worse the further the
// entity is from the camera's rotation axis).
//
// Horizontal pivot uses CAMERA RIGHT (so left/right offset stays
// meaningful in screen space — pivot.x = 0 means "left edge of quad",
// independent of which world direction is "left" right now).
static XMFLOAT3 ApplyPivot(const XMFLOAT3& worldPos,
                           const XMFLOAT3& cameraRightWS,
                           const XMFLOAT3& /*cameraUpWS — unused, see header comment*/,
                           float halfW, float halfH,
                           const XMFLOAT2& pivot)
{
    XMVECTOR p     = XMLoadFloat3(&worldPos);
    XMVECTOR right = XMLoadFloat3(&cameraRightWS);
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    p = XMVectorAdd(p, XMVectorScale(right,    (1.0f - 2.0f * pivot.x) * halfW));
    p = XMVectorAdd(p, XMVectorScale(worldUp,  (2.0f * pivot.y - 1.0f) * halfH));
    XMFLOAT3 out;
    XMStoreFloat3(&out, p);
    return out;
}

void WorldUIBillboardPass::Execute(RHI::CommandList cl,
                                    World& world,
                                    const XMFLOAT4X4& viewProjMatrix,
                                    const XMFLOAT4X4& viewMatrix,
                                    const RHI::Texture* target,
                                    RHI::ResourceState entryState,
                                    uint32_t canvasW, uint32_t canvasH)
{
    if (!enabled || !m_gfx || !target || !target->IsValid()) return;
    if (canvasW == 0 || canvasH == 0) return;
    if (!m_vbMapped) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);
    const PSODesc psoDesc = BuildPSODesc();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(psoDesc);
    if (!pso || !pso->IsValid()) return;

    // ---- Camera basis vectors (in world space) ----------------------
    // For a row-major DX view matrix, the world-space camera axes live
    // in the COLUMNS of the rotation block (see WorldSpaceUISystem.cpp
    // for derivation).
    const float* vm = reinterpret_cast<const float*>(&viewMatrix);
    XMFLOAT3 cameraRightWS  { vm[0], vm[4], vm[8] };
    XMFLOAT3 cameraUpWS     { vm[1], vm[5], vm[9] };
    XMFLOAT3 cameraForwardWS{ vm[2], vm[6], vm[10] };

    // ---- Build vertex buffer from ECS pools -------------------------
    auto* wsPool = world.GetPool<UI::WorldSpaceUIComponent>();
    if (!wsPool || wsPool->Size() == 0) return;

    // Working vector — uploads into the persistently-mapped buffer at end.
    static thread_local std::vector<WorldUIVertex> verts;
    verts.clear();
    verts.reserve(64);

    // Single bindless batch — every quad writes its own bindless texIdx.
    // Bar / border use kInvalidTexIdx (PS short-circuits).  Text uses the
    // font atlas's bindless index.  Image uses the component-supplied
    // bindless index. ONE bind of the bindless table, ONE DrawInstanced.
    const uint32_t fontTexIdx = UI::DefaultFont().AtlasBindlessIndex();

    const auto& wsEnts = wsPool->Entities();
    auto&       wsData = wsPool->Data();

    for (size_t i = 0; i < wsData.size(); ++i)
    {
        const UI::WorldSpaceUIComponent& ws = wsData[i];
        if (ws.isCulled || ws.computedAlpha <= 0.001f) continue;

        const Entity e = wsEnts[i];
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        if (!gt) continue;

        XMFLOAT3 worldPos{
            gt->matrix.m[3][0],
            gt->matrix.m[3][1],
            gt->matrix.m[3][2],
        };
        if (auto* dn = world.GetComponent<UI::DamageNumberComponent>(e))
        {
            worldPos.x += dn->currentOffset.x;
            worldPos.y += dn->currentOffset.y;
            worldPos.z += dn->currentOffset.z;
        }
        // Apply camera-relative anchor offset:
        //   .x → cameraRightWS    (positive = camera's right side)
        //   .y → world up          (vertical stays fixed in world Y)
        //   .z → cameraForwardWS   (positive = away from camera)
        // So "0.3 to the right" stays 0.3 to the camera's right even when
        // the camera orbits to the entity's back.
        const auto& sso = ws.screenSpaceOffset;
        worldPos.x += cameraRightWS.x * sso.x + cameraForwardWS.x * sso.z;
        worldPos.y += cameraRightWS.y * sso.x + cameraForwardWS.y * sso.z + sso.y;
        worldPos.z += cameraRightWS.z * sso.x + cameraForwardWS.z * sso.z;

        float halfW, halfH;
        WorldUIQuadHalfSize(ws, halfW, halfH);
        const XMFLOAT3 quadCenter = ApplyPivot(worldPos, cameraRightWS, cameraUpWS,
                                                halfW, halfH, ws.pivot);

        // ---- Bar / border (texIdx = sentinel) -----------------------------
        if (auto* bar = world.GetComponent<UI::WorldUIBarComponent>(e))
        {
            if (bar->visible && verts.size() + 36 <= kMaxVertices)
            {
                const uint32_t bg = PackColor(bar->backgroundColor, ws.computedAlpha);
                EmitQuad(verts, quadCenter, cameraRightWS, cameraUpWS,
                          halfW, halfH, 0,0,0,0, bg, kInvalidTexIdx);

                const float v = std::clamp(bar->value, 0.f, 1.f);
                if (v > 0.001f)
                {
                    const float fillHalfW = halfW * v;
                    XMVECTOR cv = XMLoadFloat3(&quadCenter);
                    cv = XMVectorSubtract(cv,
                        XMVectorScale(XMLoadFloat3(&cameraRightWS), halfW - fillHalfW));
                    XMFLOAT3 fillCenter; XMStoreFloat3(&fillCenter, cv);
                    const uint32_t fc = PackColor(bar->fillColor, ws.computedAlpha);
                    EmitQuad(verts, fillCenter, cameraRightWS, cameraUpWS,
                              fillHalfW, halfH, 0,0,0,0, fc, kInvalidTexIdx);
                }

                if (bar->borderThick > 0.f)
                {
                    const float t = std::min(bar->borderThick * 0.5f, std::min(halfW, halfH) * 0.5f);
                    const uint32_t bd = PackColor(bar->borderColor, ws.computedAlpha);

                    XMVECTOR center = XMLoadFloat3(&quadCenter);
                    XMVECTOR top = XMVectorAdd(center,
                        XMVectorScale(XMLoadFloat3(&cameraUpWS), halfH - t));
                    XMFLOAT3 c; XMStoreFloat3(&c, top);
                    EmitQuad(verts, c, cameraRightWS, cameraUpWS, halfW, t, 0,0,0,0, bd, kInvalidTexIdx);

                    XMVECTOR bot = XMVectorSubtract(center,
                        XMVectorScale(XMLoadFloat3(&cameraUpWS), halfH - t));
                    XMStoreFloat3(&c, bot);
                    EmitQuad(verts, c, cameraRightWS, cameraUpWS, halfW, t, 0,0,0,0, bd, kInvalidTexIdx);

                    XMVECTOR left = XMVectorSubtract(center,
                        XMVectorScale(XMLoadFloat3(&cameraRightWS), halfW - t));
                    XMStoreFloat3(&c, left);
                    EmitQuad(verts, c, cameraRightWS, cameraUpWS, t, halfH, 0,0,0,0, bd, kInvalidTexIdx);

                    XMVECTOR right = XMVectorAdd(center,
                        XMVectorScale(XMLoadFloat3(&cameraRightWS), halfW - t));
                    XMStoreFloat3(&c, right);
                    EmitQuad(verts, c, cameraRightWS, cameraUpWS, t, halfH, 0,0,0,0, bd, kInvalidTexIdx);
                }
            }
        }

        // ---- Text (texIdx = font atlas bindless slot) ---------------------
        if (fontTexIdx != kInvalidTexIdx)
        {
            const std::string* str    = nullptr;
            const XMFLOAT4*    col    = nullptr;
            float              tScale = 1.f;
            if (auto* tc = world.GetComponent<UI::WorldUITextComponent>(e))
            {
                if (tc->visible && !tc->text.empty()) {
                    str    = &tc->text;
                    col    = &tc->color;
                    tScale = tc->scale;
                }
            }
            else if (auto* dn = world.GetComponent<UI::DamageNumberComponent>(e))
            {
                if (!dn->text.empty()) { str = &dn->text; col = &dn->color; }
            }
            if (str && col)
            {
                UI::Font& font = UI::DefaultFont();
                const float pixelLineH = std::max(1.0f, font.Metrics().lineHeight);
                const float worldPerPixel = (halfH * 2.0f) * tScale / pixelLineH;
                const UI::Vec2 sizePx = font.MeasureText(str->c_str());
                const float worldTextWidth = sizePx.x * worldPerPixel;

                XMVECTOR penWS = XMLoadFloat3(&quadCenter);
                penWS = XMVectorSubtract(penWS,
                    XMVectorScale(XMLoadFloat3(&cameraRightWS), worldTextWidth * 0.5f));
                const float asc = font.Metrics().ascender;
                penWS = XMVectorSubtract(penWS,
                    XMVectorScale(XMLoadFloat3(&cameraUpWS), (asc * worldPerPixel) - halfH));

                const uint32_t cc = PackColor(*col, ws.computedAlpha);
                const auto& glyphs = font.Glyphs();
                const char* p = str->c_str();
                while (*p)
                {
                    uint32_t cp;
                    const uint8_t b0 = static_cast<uint8_t>(*p);
                    if (b0 < 0x80u) { cp = b0; ++p; }
                    else if ((b0 & 0xE0u) == 0xC0u && p[1])
                    { cp = ((b0 & 0x1Fu) << 6) | (uint8_t(p[1]) & 0x3Fu); p += 2; }
                    else if ((b0 & 0xF0u) == 0xE0u && p[1] && p[2])
                    { cp = ((b0 & 0x0Fu) << 12) | ((uint8_t(p[1]) & 0x3Fu) << 6) | (uint8_t(p[2]) & 0x3Fu); p += 3; }
                    else { cp = 0xFFFDu; ++p; }

                    if (cp == '\n') continue;
                    auto it = glyphs.find(cp);
                    if (it == glyphs.end()) continue;
                    const UI::FontGlyph& g = it->second;

                    if (g.size.x > 0.f && g.size.y > 0.f)
                    {
                        const float gw = g.size.x * worldPerPixel * 0.5f;
                        const float gh = g.size.y * worldPerPixel * 0.5f;
                        XMVECTOR center = penWS;
                        center = XMVectorAdd(center,
                            XMVectorScale(XMLoadFloat3(&cameraRightWS),
                                          (g.bearing.x + g.size.x * 0.5f) * worldPerPixel));
                        center = XMVectorAdd(center,
                            XMVectorScale(XMLoadFloat3(&cameraUpWS),
                                          (g.bearing.y - g.size.y * 0.5f) * worldPerPixel));
                        XMFLOAT3 cc3; XMStoreFloat3(&cc3, center);
                        if (verts.size() + 6 > kMaxVertices) break;
                        EmitQuad(verts, cc3, cameraRightWS, cameraUpWS, gw, gh,
                                  g.uv0.x, g.uv0.y, g.uv1.x, g.uv1.y, cc, fontTexIdx);
                    }
                    penWS = XMVectorAdd(penWS,
                        XMVectorScale(XMLoadFloat3(&cameraRightWS), g.advance * worldPerPixel));
                }
            }
        }

        // ---- Image (texIdx = component-supplied bindless slot) ------------
        if (auto* img = world.GetComponent<UI::WorldUIImageComponent>(e))
        {
            if (img->visible && img->bindlessIndex != ~0u && verts.size() + 6 <= kMaxVertices)
            {
                const uint32_t cc = PackColor(img->tint, ws.computedAlpha);
                EmitQuad(verts, quadCenter, cameraRightWS, cameraUpWS,
                          halfW, halfH,
                          img->uv0.x, img->uv0.y, img->uv1.x, img->uv1.y,
                          cc, img->bindlessIndex);
            }
        }
    }

    if (verts.empty()) return;

    // ---- Upload ----
    const size_t bytes = verts.size() * sizeof(WorldUIVertex);
    std::memcpy(m_vbMapped, verts.data(), bytes);

    // ---- Transition target → RT ----
    if (entryState != RHI::ResourceState::RENDERTARGET)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            target, entryState, RHI::ResourceState::RENDERTARGET), cl);
    }

    const RHI::Texture* rtArr[1] = { target };
    gfx.SetRenderTargets(1, rtArr, nullptr, cl);

    RHI::Viewport vp{};
    vp.top_left_x = 0; vp.top_left_y = 0;
    vp.width  = static_cast<float>(canvasW);
    vp.height = static_cast<float>(canvasH);
    vp.min_depth = 0.f; vp.max_depth = 1.f;
    gfx.SetViewport(vp, cl);
    gfx.SetScissorRect(0, 0, canvasW, canvasH, cl);

    gfx.BindDescriptorHeaps(cl);
    gfx.BindPipelineState(*pso, cl);
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, cl);

    // Engine PerView CB at b1 space0 (slot 0 = root param 1).  The
    // engine binds this earlier in the frame; we don't need to rebind.
    // But we DO need our own dummy CB so the slot has a valid binding —
    // PerView is shared with mesh draws and already bound.  Skip.

    // Upload our own un-jittered viewProj into m_cb (so UI doesn't dance
    // with TAA jitter). Engine's PerViewCB has the jittered variant.
    if (m_cbMapped)
    {
        // Shader reads as float4x4 viewProj at register(b1).  HLSL
        // expects column-major when storing matrices in cbuffer; the
        // engine stores its CB matrices transposed (DirectX-row-vector
        // convention with HLSL column-major buffer).  Match that by
        // transposing on upload.
        XMMATRIX m = XMLoadFloat4x4(&viewProjMatrix);
        XMFLOAT4X4 transposed;
        XMStoreFloat4x4(&transposed, XMMatrixTranspose(m));
        std::memcpy(m_cbMapped, &transposed, sizeof(XMFLOAT4X4));
    }
    gfx.BindConstantBuffer(m_cb, 0, cl);   // b1 space0
    if (m_samplerIdx >= 0) gfx.BindSampler(m_samplerIdx, 0, cl);

    const uint64_t vbHandle = gfx.GetBufferSRVGpuHandle(m_vertexBuffer);
    if (vbHandle) gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbHandle, cl);

    // Bind the engine's bindless texture table at t0 space2 — covers the
    // font atlas + every WorldUIImage SRV in one shot. Per-vertex texIdx
    // selects which descriptor each pixel samples.
    if (const uint64_t bindlessTbl = gfx.GetBindlessTextureTableGpuHandle())
        gfx.BindDescriptorTableGpuHandle(kRootSlot_BindlessTex, bindlessTbl, cl);

    gfx.DrawInstanced(static_cast<uint32_t>(verts.size()), 1, 0, 0, cl);

    // ---- Transition back to SR ----
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        target,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}

void WorldUIBillboardPass::ReloadShaders()
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
}
