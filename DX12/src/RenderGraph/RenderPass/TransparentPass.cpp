#include "RenderGraph/RenderPass/TransparentPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

// Root slots shared with GBufferPass / SkyboxPass (must match GraphicsDX12.cpp).
static constexpr uint32_t kInstanceBufSlot   =  8;
static constexpr uint32_t kMeshDescSlot      =  9;
static constexpr uint32_t kBindlessSlot      = 14;
static constexpr uint32_t kIBLIrradianceSlot = 19;   // t6 space0
static constexpr uint32_t kIBLRadianceSlot   = 20;   // t7 space0
static constexpr uint32_t kBRDFLUTSlot       = 21;   // t8 space0
static constexpr uint32_t kBindlessTexSlot   = 27;   // t0 space2 — bindless texture array
// Forward-IBL extension slots (must match LightingPass.cpp constants).
static constexpr uint32_t kSkySHSRVSlot              = 31;  // t19 space0
static constexpr uint32_t kReflectionProbeArraySlot  = 37;  // t23 space0
static constexpr uint32_t kReflectionProbeBufferSlot = 38;  // t24 space0

// ---------------------------------------------------------------------------
TransparentPass::TransparentPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

// ---------------------------------------------------------------------------
void TransparentPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth is read-only (depth-test but no write); declare WriteDepthStencil
    // so the graph emits the SHADER_RESOURCE → DEPTHSTENCIL barrier (same as SkyboxPass).
    b.WriteDepthStencil(m_depth);
    // We manually bind the HDR RTV in Execute (not a graph-managed texture).
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// ---------------------------------------------------------------------------
void TransparentPass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS,    RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    m_shaderLib.Register(ShaderID::Transparent_PS, RHI::ShaderStage::PS, "Transparent.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    // Warm up the base alpha-blend PSO immediately.
    PermutationKey basePerm;
    basePerm.Set(PermutationKey::ALPHA_BLEND, true);
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm)))
        LOG_ERROR("TransparentPass: base PSO creation failed");
    else
        LOG_INFO("TransparentPass: base PSO ready");

    // 1×1 white fallback for base-color and surface-map slots.
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t white = 0xFFFFFFFFu;
        RHI::SubresourceData init{ &white, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultWhite, &init))
            m_defaultWhiteGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultWhite);
        else
            LOG_ERROR("TransparentPass: default white texture failed");
    }

    // 1×1 flat-normal fallback (128,128,255,255).
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t flat = 0xFFFF8080u;
        RHI::SubresourceData init{ &flat, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultFlatNormal, &init))
            m_defaultFlatNormalGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultFlatNormal);
        else
            LOG_ERROR("TransparentPass: flat-normal texture failed");
    }

    // s0: aniso-wrap for material textures. Match GBufferPass so alpha-
    // blended surfaces (glass, hair cards, particles rendered as meshes)
    // agree with opaque TAA integration — same mip-bias / anisotropy.
    {
        RHI::SamplerDesc sd;
        sd.filter         = RHI::Filter::ANISOTROPIC;
        sd.max_anisotropy = 8;
        sd.mip_lod_bias   = -1.0f;   // match GBufferPass — TAA-sharpened mips
        if (!gfx.CreateSampler(sd, m_linearSamplerIdx))
            LOG_ERROR("TransparentPass: linear sampler failed");
    }

    // s1: trilinear-wrap for IBL cubemaps.
    {
        RHI::SamplerDesc sd;
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.address_u = RHI::TextureAddressMode::WRAP;
        sd.address_v = RHI::TextureAddressMode::WRAP;
        sd.address_w = RHI::TextureAddressMode::WRAP;
        if (!gfx.CreateSampler(sd, m_iblSamplerIdx))
            LOG_ERROR("TransparentPass: IBL sampler failed");
    }
}

// ---------------------------------------------------------------------------
PSODesc TransparentPass::BuildPSODesc(PermutationKey perm, uint32_t customPSID) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::GBuffer_VS;
    desc.psID        = (customPSID != 0)
                        ? static_cast<ShaderID>(customPSID)
                        : ShaderID::Transparent_PS;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;  // PVF

    // Cull back faces by default; DOUBLE_SIDED → NONE.
    desc.rs.cull_mode         = perm.Has(PermutationKey::DOUBLE_SIDED)
                                ? RHI::CullMode::NONE
                                : RHI::CullMode::BACK;
    // Global view-mode wireframe (PSOCache hashes rs.fill_mode → distinct PSO).
    desc.rs.fill_mode         = m_wireframeMode ? RHI::FillMode::WIREFRAME
                                                : RHI::FillMode::SOLID;
    desc.rs.depth_clip_enable = true;

    // Depth test ON, depth write OFF — transparent objects don't update depth.
    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ZERO;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    // Blend state — selected by permutation.
    auto& rt = desc.bs.render_target[0];
    rt.render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;
    rt.blend_enable             = true;

    if (perm.Has(PermutationKey::ADDITIVE_BLEND))
    {
        // Additive: src*ONE + dst*ONE
        rt.src_blend       = RHI::Blend::ONE;
        rt.dest_blend      = RHI::Blend::ONE;
        rt.blend_op        = RHI::BlendOp::ADD;
        rt.src_blend_alpha = RHI::Blend::ONE;
        rt.dest_blend_alpha= RHI::Blend::ONE;
        rt.blend_op_alpha  = RHI::BlendOp::ADD;
    }
    else if (perm.Has(PermutationKey::PREMULTIPLIED_BLEND))
    {
        // Premultiplied alpha: src*ONE + dst*INV_SRC_ALPHA
        rt.src_blend       = RHI::Blend::ONE;
        rt.dest_blend      = RHI::Blend::INV_SRC_ALPHA;
        rt.blend_op        = RHI::BlendOp::ADD;
        rt.src_blend_alpha = RHI::Blend::ONE;
        rt.dest_blend_alpha= RHI::Blend::INV_SRC_ALPHA;
        rt.blend_op_alpha  = RHI::BlendOp::ADD;
    }
    else if(perm.Has(PermutationKey::MULTIPLY_BLEND))
    {
        // Multiply: src*DST_COLOR + dst*INV_SRC_ALPHA
        //   alpha=1 → src*dst     (full photoshop multiply)
        //   alpha=0 → dst         (transparent, leaves scene untouched)
        //   between → lerp(dst, src*dst, alpha)
        // Using DEST_COLOR/ZERO discards dst entirely and makes the alpha
        // slider inert — with INV_SRC_ALPHA the opacity control works.
        rt.src_blend       = RHI::Blend::DEST_COLOR;
        rt.dest_blend      = RHI::Blend::INV_SRC_ALPHA;
        rt.blend_op        = RHI::BlendOp::ADD;
        rt.src_blend_alpha = RHI::Blend::ONE;
        rt.dest_blend_alpha= RHI::Blend::INV_SRC_ALPHA;
        rt.blend_op_alpha  = RHI::BlendOp::ADD;
    }
    else
    {
        // Alpha blend (default): src*SRC_ALPHA + dst*INV_SRC_ALPHA
        rt.src_blend       = RHI::Blend::SRC_ALPHA;
        rt.dest_blend      = RHI::Blend::INV_SRC_ALPHA;
        rt.blend_op        = RHI::BlendOp::ADD;
        rt.src_blend_alpha = RHI::Blend::ONE;
        rt.dest_blend_alpha= RHI::Blend::INV_SRC_ALPHA;
        rt.blend_op_alpha  = RHI::BlendOp::ADD;
    }

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;
    return desc;
}

// ---------------------------------------------------------------------------
RHI::CommandList TransparentPass::Execute(RHI::CommandList cl)
{
    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Transparent);
    if (draws.empty()) return cl;

    // Bind the HDR RTV with the GBuffer depth (read-only depth test, no write).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl);

    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const RHI::PipelineState* activePSO = nullptr;

    auto bindGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindBufferSRVByName(kInstanceBufSlot, "InstanceBuffer");
        cl.BindBufferSRVByName(kMeshDescSlot,    "MeshDescriptors");
        if (bindlessHandle)
            cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);

        // CB slot 0 (b1 space0) = PerView — used by GBuffer.vs.hlsl.
        cl.BindCBByName(0, "PerView");
        // CB slot 1 (b2 space0) = LightCB — used by Transparent.ps.hlsl.
        cl.BindCBByName(1, "LightCB");

        // Material buffer at t2 space0 (slot 0 of BindBufferSRV).
        const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer("MaterialBuffer");
        if (matBuf && matBuf->IsValid())
            cl.BindBufferSRV(0, *matBuf);

        // Samplers.
        if (m_linearSamplerIdx >= 0) cl.BindSampler(0, m_linearSamplerIdx);
        if (m_iblSamplerIdx    >= 0) cl.BindSampler(1, m_iblSamplerIdx);

        // IBL resources.
        if (m_iblIrradianceHandle) cl.BindDescriptorTableHandle(kIBLIrradianceSlot, m_iblIrradianceHandle);
        if (m_iblRadianceHandle)   cl.BindDescriptorTableHandle(kIBLRadianceSlot,   m_iblRadianceHandle);
        if (m_brdfLutHandle)       cl.BindDescriptorTableHandle(kBRDFLUTSlot,       m_brdfLutHandle);

        // Sky SH coefficients — same buffer the deferred LightingPass reads.
        // When unset (handle=0) the LightCB.iblUseSH flag will also be 0, so
        // the shader takes the irradiance-cubemap branch and never touches
        // gSkySH — but we still need a valid SRV bound to satisfy root sig.
        if (m_skySHHandle) cl.BindDescriptorTableHandle(kSkySHSRVSlot, m_skySHHandle);

        // Reflection probe pool. Wired at Compile() time; handles persist for
        // the lifetime of the Renderer. reflectionProbeCount == 0 in LightCB
        // makes the shader skip the loop — array reads are bounds-safe.
        if (m_reflectionProbeArrayHandle)
            cl.BindDescriptorTableHandle(kReflectionProbeArraySlot,  m_reflectionProbeArrayHandle);
        if (m_reflectionProbeBufferHandle)
            cl.BindDescriptorTableHandle(kReflectionProbeBufferSlot, m_reflectionProbeBufferHandle);

        // Seed texture slots 11, 12, 13 with defaults so they are never unbound.
        if (m_defaultWhiteGpuHandle)
        {
            cl.BindDescriptorTableHandle(11, m_defaultWhiteGpuHandle);
            cl.BindDescriptorTableHandle(12, m_defaultWhiteGpuHandle);
        }
        if (m_defaultFlatNormalGpuHandle)
            cl.BindDescriptorTableHandle(13, m_defaultFlatNormalGpuHandle);

        // Bind bindless texture table (for emissive map sampling via textureHandleIds).
        auto& dx12 = static_cast<GraphicsDX12&>(*cl.gfx);
        D3D12_GPU_DESCRIPTOR_HANDLE texTable = dx12.GetBindlessTextureTableHandle();
        if (texTable.ptr)
            cl.BindDescriptorTableHandle(kBindlessTexSlot, texTable.ptr);
    };

    auto& dx12 = static_cast<GraphicsDX12&>(*cl.gfx);

    for (const DrawPacket& dp : draws)
    {
        const RHI::PipelineState* pso =
            m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID));
        if (!pso || !pso->IsValid()) continue;

        if (pso != activePSO)
        {
            cl.SetPipelineState(*pso);
            activePSO = pso;
            bindGlobals();
        }

        // Per-draw texture slots (base color=11, surface=12, normal=13).
        {
            const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor  : m_defaultWhiteGpuHandle;
            const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap : m_defaultWhiteGpuHandle;
            const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap  : m_defaultFlatNormalGpuHandle;
            if (bc) cl.BindDescriptorTableHandle(11, bc);
            if (sm) cl.BindDescriptorTableHandle(12, sm);
            if (nm) cl.BindDescriptorTableHandle(13, nm);
        }

        // Phase E custom material CBV (b8 space0) and Phase F custom texture
        // table (t0..t3 space3) — mirror GBufferPass so a single material can
        // drive both opaque AND additive draws with the same custom PS.
        if (dp.customCbvVA)
            dx12.BindCustomMaterialCBV(dp.customCbvVA, cl);
        if (dp.customTexTable)
            dx12.BindCustomMaterialTextureTable(dp.customTexTable, cl);

        cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }

    return cl;
}
