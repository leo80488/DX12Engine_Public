#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

namespace
{
    // Mirror of SkyIBLPass's compute root sig slots — same shared compute
    // signature is used here.
    constexpr uint32_t kCSCBSlot = 0; // b0 space2
    constexpr uint32_t kCSSrv0   = 1; // t0 space2
    constexpr uint32_t kCSUav0   = 4; // u0 space2

    // Master graphics root sig slot conventions (must mirror GraphicsDX12).
    constexpr uint32_t kRootConstantsSlot   = 0;
    constexpr uint32_t kCBVSlotBase         = 1; // b1..b7
    constexpr uint32_t kInstanceBufSlot     = 8; // root SRV  t0 space0
    constexpr uint32_t kMeshDescSlot        = 9; // root SRV  t1 space0
    constexpr uint32_t kSRVSlotBase         = 10; // desc tables t2..t5 space0
    constexpr uint32_t kSamplerSlotBase     = 15;
    constexpr uint32_t kBindlessBufSlot     = 14; // t0 space1 — PVF g_Buffers[]
    constexpr uint32_t kBindlessTexSlot     = 27; // t0 space2
    constexpr uint32_t kSkyCubeSRVSlot      = 19; // t6 space0  — sky cube for capture sky PS
    constexpr uint32_t kSkySHSRVSlot        = 31; // t19 space0
    constexpr uint32_t kShadowCBSlot        = 1;  // b2 space0  — ProbeShadowCB (slot 1 → b2)
    constexpr uint32_t kShadowSRVSlot       = 22; // t9 space0  — CSM Texture2DArray (root slot 22)
    constexpr uint32_t kShadowSamplerSlot   = 2;  // s2         — comparison sampler (slot 2 → s2)

    // Karis sample counts per output mip (matches SkyIBLPass).
    constexpr uint32_t kPrefilterSampleTable[7] = { 1, 128, 128, 64, 32, 32, 32 };
    constexpr uint32_t kCBSlotStride = 256;       // D3D12 CBV alignment

    struct alignas(16) CaptureCB
    {
        float    viewProj[16];
        float    prevViewProj[16];          // = viewProj (no TAA)
        float    curViewProjNoJitter[16];   // = viewProj
        float    sunDir[3];     float pad0;
        float    sunColor[3];   float pad1;
        float    cameraPos[3];  float pad3;
    };
    static_assert(sizeof(CaptureCB) <= kCBSlotStride, "CaptureCB exceeds 256-byte slot");

    // Frame-global shadow + ambient controls bound at b2. cascadeVP matches
    // LightCB.shadowMatrix (transposed) so the capture PS reuses the same
    // row-vector mul + cascade-select math the deferred pass uses.
    struct alignas(16) ProbeShadowCB
    {
        float    cascadeVP[4][16];                   // 256 — transposed cascade view-projs
        float    cascadeSplits[4];                   // 16
        float    camPos[3];     float shadowStrength;// 16
        float    camFwd[3];     float iblStrength;   // 16
        float    ambientScale;  float pad[3];        // 16
    }; // 320 bytes
    constexpr uint32_t kShadowCBSize = 512;          // 256-aligned, holds the 320-byte struct
    static_assert(sizeof(ProbeShadowCB) <= kShadowCBSize, "ProbeShadowCB too large");

    struct alignas(16) PrefilterCB
    {
        uint32_t faceSize;
        uint32_t sampleCount;
        float    roughness;
        uint32_t sourceMipCount;
        uint32_t faceIndex;
        uint32_t pad0;
        uint32_t pad1;
        uint32_t pad2;
    };

    // Build 6 view-proj matrices for cubemap faces in DX face order:
    //   0:+X  1:-X  2:+Y  3:-Y  4:+Z  5:-Z
    // Reverse-Z perspective (near=1, far=0) to match the engine's depth convention.
    void BuildFaceViewProj(const DirectX::XMFLOAT3& probePos,
                           uint32_t face,
                           DirectX::XMFLOAT4X4& outVP)
    {
        using namespace DirectX;
        XMVECTOR eye = XMLoadFloat3(&probePos);

        XMVECTOR fwd, up;
        switch (face)
        {
        case 0: fwd = XMVectorSet( 1, 0, 0, 0); up = XMVectorSet(0, 1, 0, 0); break; // +X
        case 1: fwd = XMVectorSet(-1, 0, 0, 0); up = XMVectorSet(0, 1, 0, 0); break; // -X
        case 2: fwd = XMVectorSet( 0, 1, 0, 0); up = XMVectorSet(0, 0,-1, 0); break; // +Y
        case 3: fwd = XMVectorSet( 0,-1, 0, 0); up = XMVectorSet(0, 0, 1, 0); break; // -Y
        case 4: fwd = XMVectorSet( 0, 0, 1, 0); up = XMVectorSet(0, 1, 0, 0); break; // +Z
        default: fwd = XMVectorSet(0, 0,-1, 0); up = XMVectorSet(0, 1, 0, 0); break; // -Z
        }

        XMMATRIX view = XMMatrixLookToLH(eye, fwd, up);
        // 90° FOV per face; near 0.1, far 1000. Reverse-Z by swapping near/far.
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 1000.0f, 0.1f);
        XMMATRIX vp   = view * proj;
        // Shaders use row-vector convention: mul(float4(pos,1), viewProj).
        // Store transposed so HLSL sees it correctly (GBuffer.vs's mul-from-left).
        XMStoreFloat4x4(&outVP, XMMatrixTranspose(vp));
    }
} // namespace

bool ReflectionProbeCapturePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS,        RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    m_shaderLib.Register(ShaderID::ProbeCapture_PS,   RHI::ShaderStage::PS, "ProbeCapture.ps.hlsl");
    m_shaderLib.Register(ShaderID::Skybox_VS,         RHI::ShaderStage::VS, "Skybox.vs.hlsl");
    m_shaderLib.Register(ShaderID::ProbeCaptureSky_PS, RHI::ShaderStage::PS, "ProbeCaptureSky.ps.hlsl");
    m_shaderLib.Register(ShaderID::SpecularPrefilter_CS, RHI::ShaderStage::CS, "SpecularPrefilter.cs.hlsl");

    // ---- Capture graphics PSO ------------------------------------------------
    {
        const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::GBuffer_VS);
        const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::ProbeCapture_PS);
        if (!vs || !ps)
        {
            LOG_ERROR("ReflectionProbeCapturePass: shader load failed");
            return false;
        }
        // PipelineStateDesc holds POINTERS to state structs — keep them alive
        // until CreatePipelineState returns by stack-allocating below.
        static RHI::RasterizerState   rs{};
        static RHI::DepthStencilState dss{};
        static RHI::BlendState        bs{};
        rs.cull_mode         = RHI::CullMode::BACK;
        rs.depth_clip_enable = true;
        dss.depth_enable     = true;
        dss.depth_write_mask = RHI::DepthWriteMask::ALL;
        dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL; // reverse Z
        dss.stencil_enable   = false;
        bs.render_target[0].render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

        RHI::PipelineStateDesc d{};
        d.vs = vs;
        d.ps = ps;
        d.rs  = &rs;
        d.dss = &dss;
        d.bs  = &bs;
        d.rtv_formats[0] = RHI::Format::R16G16B16A16_FLOAT;
        d.rtv_count      = 1;
        d.dsv_format     = RHI::Format::D32_FLOAT;
        if (!gfx.CreatePipelineState(d, m_capturePSO))
        {
            LOG_ERROR("ReflectionProbeCapturePass: capture PSO creation failed");
            return false;
        }
    }

    // ---- Skybox PSO ----------------------------------------------------------
    // Skybox.vs forces clip Z = 0 (reverse-Z far plane) so we use depth_func
    // GREATER_EQUAL with depth_write OFF — sky lands only on pixels the
    // opaque pass left at the cleared far plane (Z = 0). No backface cull
    // because the cube is drawn from inside.
    {
        const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::Skybox_VS);
        const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::ProbeCaptureSky_PS);
        if (!vs || !ps)
        {
            LOG_ERROR("ReflectionProbeCapturePass: skybox shader load failed");
            return false;
        }
        static RHI::RasterizerState   skyRS{};
        static RHI::DepthStencilState skyDSS{};
        static RHI::BlendState        skyBS{};
        skyRS.cull_mode         = RHI::CullMode::NONE;     // viewer is inside the cube
        skyRS.depth_clip_enable = true;
        skyDSS.depth_enable     = true;
        skyDSS.depth_write_mask = RHI::DepthWriteMask::ZERO; // never overwrite opaque depth
        skyDSS.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL;
        skyDSS.stencil_enable   = false;
        skyBS.render_target[0].render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

        RHI::PipelineStateDesc d{};
        d.vs  = vs;
        d.ps  = ps;
        d.rs  = &skyRS;
        d.dss = &skyDSS;
        d.bs  = &skyBS;
        d.rtv_formats[0] = RHI::Format::R16G16B16A16_FLOAT;
        d.rtv_count      = 1;
        d.dsv_format     = RHI::Format::D32_FLOAT;
        if (!gfx.CreatePipelineState(d, m_skyPSO))
        {
            LOG_ERROR("ReflectionProbeCapturePass: sky PSO creation failed");
            return false;
        }
    }

    // ---- Prefilter compute PSO -----------------------------------------------
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SpecularPrefilter_CS);
        if (!cs) { LOG_ERROR("ReflectionProbeCapturePass: prefilter CS missing"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_prefilterPSO))
        {
            LOG_ERROR("ReflectionProbeCapturePass: prefilter PSO creation failed");
            return false;
        }
    }

    // ---- Temp single-cube capture target -------------------------------------
    {
        RHI::TextureDesc td{};
        td.width      = kFaceSize;
        td.height     = kFaceSize;
        td.array_size = 6;
        td.mip_levels = 1;            // capture writes mip 0; prefilter sources only mip 0
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::RENDER_TARGET;
        td.usage      = RHI::Usage::DEFAULT;
        td.misc_flags = RHI::ResourceMiscFlag::TEXTURECUBE;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;
        td.debug_name = "ReflectionProbeCapture.TempCube";
        if (!gfx.CreateTexture(td, m_tempCube))
        {
            LOG_ERROR("ReflectionProbeCapturePass: temp cube creation failed");
            return false;
        }
    }

    // ---- Depth buffer (D32, 128x128, single 2D) ------------------------------
    {
        RHI::TextureDesc td{};
        td.width      = kFaceSize;
        td.height     = kFaceSize;
        td.array_size = 1;
        td.mip_levels = 1;
        td.format     = RHI::Format::D32_FLOAT;
        td.bind_flags = RHI::BindFlag::DEPTH_STENCIL;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::DEPTHSTENCIL;
        td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0); // reverse Z far = 0
        td.debug_name = "ReflectionProbeCapture.Depth";
        if (!gfx.CreateTexture(td, m_depthBuffer))
        {
            LOG_ERROR("ReflectionProbeCapturePass: depth buffer creation failed");
            return false;
        }
    }

    // ---- CB rings (UPLOAD heap, persistent map) ------------------------------
    auto makeCB = [&](RHI::GPUBuffer& cb, void*& mapped, uint64_t size, const char* label)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = size;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (!gfx.CreateBuffer(bd, cb))
        {
            LOG_ERROR("ReflectionProbeCapturePass: %s CB creation failed", label);
            return false;
        }
        mapped = gfx.MapBuffer(cb);
        if (mapped) std::memset(mapped, 0, size);
        return true;
    };
    if (!makeCB(m_captureCB,   m_captureCBMapped,   kCBSlotStride * 6,            "capture"))   return false;
    if (!makeCB(m_prefilterCB, m_prefilterCBMapped, kCBSlotStride * kFaceMips * 6, "prefilter")) return false;
    if (!makeCB(m_shadowCB,    m_shadowCBMapped,    kShadowCBSize,                "shadow"))     return false;

    // Linear-wrap sampler for material base-colour sampling (mirrors GBuffer's s0).
    {
        RHI::SamplerDesc sd;
        if (!gfx.CreateSampler(sd, m_linearSamplerIdx))
            LOG_ERROR("ReflectionProbeCapturePass: linear sampler creation failed");
    }
    // Comparison sampler for CSM sun shadows (reversed-Z → GREATER_EQUAL),
    // mirrors LightingPass::m_shadowSampler so the bake matches deferred shadows.
    {
        RHI::SamplerDesc sd;
        sd.filter          = RHI::Filter::COMPARISON_MIN_MAG_MIP_LINEAR;
        sd.address_u       = RHI::TextureAddressMode::BORDER;
        sd.address_v       = RHI::TextureAddressMode::BORDER;
        sd.address_w       = RHI::TextureAddressMode::BORDER;
        sd.border_color    = RHI::SamplerBorderColor::OPAQUE_WHITE;
        sd.comparison_func = RHI::ComparisonFunc::GREATER_EQUAL;
        if (!gfx.CreateSampler(sd, m_shadowSamplerIdx))
            LOG_ERROR("ReflectionProbeCapturePass: shadow comparison sampler creation failed");
    }

    LOG_INFO("ReflectionProbeCapturePass: ready (%ux%u, %u mips, temp cube + depth allocated)",
             kFaceSize, kFaceSize, kFaceMips);
    return true;
}

// ---------------------------------------------------------------------------
void ReflectionProbeCapturePass::DrawSceneFace(RHI::CommandList cl,
                                                uint32_t face,
                                                const BakeContext& ctx)
{
    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    auto* nativeCL = dx12.GetNativeCommandList(cl);

    // Pack and upload the per-face capture CB at this face's ring slot.
    if (m_captureCBMapped)
    {
        CaptureCB cb{};
        DirectX::XMFLOAT4X4 vp;
        BuildFaceViewProj(ctx.probePos, face, vp);
        std::memcpy(cb.viewProj,            &vp, sizeof(vp));
        std::memcpy(cb.prevViewProj,        &vp, sizeof(vp));
        std::memcpy(cb.curViewProjNoJitter, &vp, sizeof(vp));
        cb.sunDir[0]    = ctx.sunDir.x;   cb.sunDir[1]    = ctx.sunDir.y;   cb.sunDir[2]    = ctx.sunDir.z;
        cb.sunColor[0]  = ctx.sunColor.x; cb.sunColor[1]  = ctx.sunColor.y; cb.sunColor[2]  = ctx.sunColor.z;
        cb.cameraPos[0] = ctx.probePos.x; cb.cameraPos[1] = ctx.probePos.y; cb.cameraPos[2] = ctx.probePos.z;
        std::memcpy(static_cast<uint8_t*>(m_captureCBMapped) + face * kCBSlotStride,
                    &cb, sizeof(cb));
    }

    // Bind RTV (temp cube face) + DSV (shared depth).
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
        m_gfx->GetTextureCubeFaceRTVCpuHandle(m_tempCube, /*cubeIdx=*/0, face, /*mip=*/0)
    };
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        // The depth buffer was created with array_size=1, so the standard DSV
        // (entry.dsv) covers it. Reuse it rather than carving a per-call view.
        dx12.GetTextureDsvCpuHandle(m_depthBuffer);
    nativeCL->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // Clear color (HDR black) + depth (reverse-Z far = 0).
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    nativeCL->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    nativeCL->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

    // Viewport + scissor sized to a single face.
    D3D12_VIEWPORT vp{ 0, 0, float(kFaceSize), float(kFaceSize), 0.0f, 1.0f };
    D3D12_RECT     sc{ 0, 0, LONG(kFaceSize), LONG(kFaceSize) };
    nativeCL->RSSetViewports(1, &vp);
    nativeCL->RSSetScissorRects(1, &sc);
    nativeCL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind capture PSO + descriptor heaps (heaps already bound at frame start
    // by the engine's command list pool but BindDescriptorHeaps is idempotent).
    dx12.BindPipelineState(m_capturePSO, cl);
    cl.BindDescriptorHeaps();

    // Per-face capture CB at b1 — bind via the slot-offset helper so the root
    // CBV indirection stays correct across the 6 face draws sharing one ring.
    dx12.BindConstantBufferAtOffset(/*slot=*/0, m_captureCB,
                                    face * kCBSlotStride, cl);

    // PVF root SRVs (instance / mesh descriptor tables).
    if (ctx.instanceBuffer && ctx.instanceBuffer->IsValid())
        dx12.SetRootBufferSRV(*ctx.instanceBuffer, kInstanceBufSlot, cl);
    if (ctx.meshDescBuffer && ctx.meshDescBuffer->IsValid())
        dx12.SetRootBufferSRV(*ctx.meshDescBuffer, kMeshDescSlot, cl);

    // Material buffer at t2 space0 (root slot 10 = kSRVSlotBase + 0).
    if (ctx.materialBufSrv)
        dx12.BindDescriptorTableGpuHandle(kSRVSlotBase + 0, ctx.materialBufSrv, cl);

    // Per-draw fallback texture slots t3/t4/t5 — root sig demands all three.
    // t3 (BaseColor) and t4 (Surface) → 1×1 white; t5 (Normal) → 1×1 flat
    // normal so unmapped pixels stay neutral instead of pointing to (1,1,1).
    if (ctx.defaultWhiteSrv)
    {
        dx12.BindDescriptorTableGpuHandle(kSRVSlotBase + 1, ctx.defaultWhiteSrv, cl);
        dx12.BindDescriptorTableGpuHandle(kSRVSlotBase + 2, ctx.defaultWhiteSrv, cl);
    }
    dx12.BindDescriptorTableGpuHandle(
        kSRVSlotBase + 3,
        ctx.defaultFlatNormSrv ? ctx.defaultFlatNormSrv : ctx.defaultWhiteSrv,
        cl);

    // Sampler s0 — linear wrap (owned by the pass, created in Init).
    if (m_linearSamplerIdx >= 0)
        dx12.BindSampler(m_linearSamplerIdx, /*slot=*/0, cl);

    // Bindless BUFFER pool — required by GBuffer.vs (PVF fetch reads vertex/
    // index data via g_Buffers[] at t0 space1). Skipping this leaves the root
    // sig slot pointing at undefined heap memory and the GPU page-faults on
    // the very first vertex fetch → DEVICE_HUNG / TDR.
    if (ctx.bindlessBufTable)
        dx12.BindDescriptorTableGpuHandle(kBindlessBufSlot, ctx.bindlessBufTable, cl);

    // Bindless texture pool + sky SH SRV (used by the capture PS for SH ambient).
    if (ctx.bindlessTexTable)
        dx12.BindDescriptorTableGpuHandle(kBindlessTexSlot, ctx.bindlessTexTable, cl);
    if (ctx.skySHSrv)
        dx12.BindDescriptorTableGpuHandle(kSkySHSRVSlot, ctx.skySHSrv, cl);

    // Shadow/ambient CB at b2 + CSM shadow array (t9) + comparison sampler (s2).
    // The capture PS shadows the direct sun and scales/AO-occludes the ambient.
    // When shadowArraySrv is 0 the PS reads shadowStrength==0 → unshadowed bake.
    dx12.BindConstantBufferAtOffset(kShadowCBSlot, m_shadowCB, 0, cl);
    if (ctx.shadowArraySrv)
        dx12.BindDescriptorTableGpuHandle(kShadowSRVSlot, ctx.shadowArraySrv, cl);
    if (m_shadowSamplerIdx >= 0)
        dx12.BindSampler(m_shadowSamplerIdx, kShadowSamplerSlot, cl);

    // Iterate opaque draws, then shadow-only draws (entities behind the camera
    // that the main frustum cull rejected but CSM shadow cull kept). Drawing
    // both unions camera-visible + sun-visible geometry so probes capture the
    // full room interior, not just what the user happens to be looking at.
    auto drawList = [&](const DrawList& list)
    {
        for (const DrawPacket& dp : list)
        {
            if (dp.texBaseColor)
                dx12.BindDescriptorTableGpuHandle(kSRVSlotBase + 1, dp.texBaseColor, cl);

            const uint32_t consts[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                dp.prevPosElementBase
            };
            nativeCL->SetGraphicsRoot32BitConstants(kRootConstantsSlot, 4, consts, 0);
            nativeCL->DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }
    };
    drawList(ctx.opaqueDraws);
    drawList(ctx.shadowDraws);
    drawList(ctx.transparentDraws);

    // ---- Skybox draw — fills uncovered (Z = 0) pixels with sky -----------
    // The sky PSO uses depth_func GREATER_EQUAL with depth_write OFF, so it
    // only writes where the opaque pass left the cleared far plane untouched.
    // No depth-write means we don't perturb the depth buffer for any
    // subsequent pass within this face.
    if (ctx.skyCubeVB && ctx.skyCubeVB->IsValid() && ctx.skyCubemapSrv)
    {
        dx12.BindPipelineState(m_skyPSO, cl);

        // Skybox.vs reads PerViewCB at b1 (already bound to the face VP) and
        // gCubeVB at t2 space0 — overwrite root slot 10 (kSRVSlotBase + 0)
        // for the duration of this draw. MaterialBuffer's binding becomes
        // stale here but the next face's DrawSceneFace re-binds it.
        dx12.BindResource(*ctx.skyCubeVB, /*slot=*/0, cl);

        // Skybox.ps reads gSkyCube at t6 space0 — bind via the lighting root
        // sig's irradiance slot (which maps to t6 space0 in the shared sig).
        dx12.BindDescriptorTableGpuHandle(kSkyCubeSRVSlot, ctx.skyCubemapSrv, cl);

        nativeCL->DrawInstanced(36, 1, 0, 0); // 12 tris × 3 verts = 36
    }
}

// ---------------------------------------------------------------------------
void ReflectionProbeCapturePass::BakeProbe(RHI::CommandList cl, uint32_t cubeIdx,
                                            const BakeContext& ctx)
{
    if (!ctx.probeArray || !ctx.probeArray->IsValid())
    {
        LOG_WARNING("ReflectionProbeCapturePass: probe array is invalid, skipping bake");
        return;
    }

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);

    // Upload the frame-global shadow/ambient CB once (bound per face at b2).
    // Identical for every probe baked this frame, so the single buffer is safe.
    if (m_shadowCBMapped)
    {
        ProbeShadowCB sb{};
        for (int c = 0; c < 4; ++c)
            std::memcpy(sb.cascadeVP[c], &ctx.cascadeVP[c], sizeof(DirectX::XMFLOAT4X4));
        sb.cascadeSplits[0] = ctx.cascadeSplits.x; sb.cascadeSplits[1] = ctx.cascadeSplits.y;
        sb.cascadeSplits[2] = ctx.cascadeSplits.z; sb.cascadeSplits[3] = ctx.cascadeSplits.w;
        sb.camPos[0] = ctx.camPos.x; sb.camPos[1] = ctx.camPos.y; sb.camPos[2] = ctx.camPos.z;
        sb.camFwd[0] = ctx.camFwd.x; sb.camFwd[1] = ctx.camFwd.y; sb.camFwd[2] = ctx.camFwd.z;
        sb.shadowStrength = (ctx.shadowArraySrv != 0) ? ctx.shadowStrength : 0.0f;
        sb.iblStrength    = ctx.iblStrength;
        sb.ambientScale   = ctx.ambientScale;
        std::memcpy(m_shadowCBMapped, &sb, sizeof(sb));
    }

    // ---- Phase 1: render 6 faces into temp cube (mip 0) ---------------------
    if (m_tempCubeState != RHI::ResourceState::RENDERTARGET)
    {
        dx12.PushBarrier(RHI::GPUBarrier::Image(
            &m_tempCube, m_tempCubeState, RHI::ResourceState::RENDERTARGET), cl);
        m_tempCubeState = RHI::ResourceState::RENDERTARGET;
    }
    for (uint32_t face = 0; face < 6; ++face)
        DrawSceneFace(cl, face, ctx);

    // ---- Phase 2: temp cube → SRV, probe array slices → UAV ----------------
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_tempCube, RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_tempCubeState = RHI::ResourceState::SHADER_RESOURCE;

    // Transition only this probe's 6 slices × all mips to UAV. mip=-1 slice=-1
    // is "all subresources" — overkill if other probes have valid SR state we
    // need to keep, but the engine doesn't currently track per-cube state on
    // the array. Per-subresource transitions would be the upgrade path.
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        ctx.probeArray, RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);

    // ---- Phase 3: prefilter dispatches into the probe's cube of the array --
    {
        dx12.BindComputePipelineState(m_prefilterPSO, cl);
        const uint64_t srcSrv = m_gfx->GetTextureSRVGpuHandle(m_tempCube);
        dx12.SetComputeDescriptorTable(kCSSrv0, srcSrv, cl);

        for (uint32_t mip = 0; mip < kFaceMips; ++mip)
        {
            const uint32_t faceW = std::max(1u, kFaceSize >> mip);
            const float    roughness = (kFaceMips <= 1)
                                       ? 0.0f : float(mip) / float(kFaceMips - 1);

            // Bind the per-(cube, mip) UAV — 6-slice view of the probe array.
            const uint64_t dstUav =
                m_gfx->GetTextureCubeMipUAVGpuHandle(*ctx.probeArray, cubeIdx, mip);
            dx12.SetComputeDescriptorTable(kCSUav0, dstUav, cl);

            // Per (face, mip) CB — same shader expects FaceIndex and dispatches
            // 1 face per call. 7×6 = 42 dispatches per probe.
            for (uint32_t face = 0; face < 6; ++face)
            {
                PrefilterCB c{};
                c.faceSize       = faceW;
                c.sampleCount    = (mip < 7) ? kPrefilterSampleTable[mip] : 32u;
                c.roughness      = roughness;
                c.sourceMipCount = 1;
                c.faceIndex      = face;

                const uint32_t slot = face * kFaceMips + mip;
                uint8_t* dst = static_cast<uint8_t*>(m_prefilterCBMapped) + slot * kCBSlotStride;
                std::memcpy(dst, &c, sizeof(c));
                dx12.SetComputeRootCBV(kCSCBSlot, m_prefilterCB, slot * kCBSlotStride, cl);

                const uint32_t gx = (faceW + 7) / 8;
                const uint32_t gy = (faceW + 7) / 8;
                dx12.DispatchCompute(gx, gy, 1, cl);

                // UAV barrier between dispatches sharing the same destination
                // resource so the GPU serializes the writes.
                dx12.PushBarrier(RHI::GPUBarrier::Memory(nullptr), cl);
            }
        }
    }

    // ---- Phase 4: probe array → SR (ready for lighting pass to sample) -----
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        ctx.probeArray, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}
