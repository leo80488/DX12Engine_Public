#include "RenderGraph/RenderPass/GBufferPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderContext.h"
#include "System/TaskSystem.h"
#include "System/Log.h"

#include <latch>
#include <algorithm>

// Root parameter slot constants (must match GraphicsDX12.cpp)
static constexpr uint32_t kInstanceBufSlot = 8;
static constexpr uint32_t kMeshDescSlot    = 9;
static constexpr uint32_t kBindlessSlot    = 14;

GBufferPass::GBufferPass(RG::RGTextureHandle albedo,
                         RG::RGTextureHandle normal,
                         RG::RGTextureHandle surface,
                         RG::RGTextureHandle depth,
                         RG::RGTextureHandle velocity,
                         RG::RGTextureHandle emissive)
    : m_albedo(albedo), m_normal(normal), m_surface(surface), m_depth(depth), m_velocity(velocity), m_emissive(emissive)
{}

void GBufferPass::Setup(RG::RenderGraphBuilder& b)
{
    b.WriteRenderTarget(m_albedo);
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    b.WriteRenderTarget(m_velocity);
    b.WriteRenderTarget(m_emissive);
    b.WriteDepthStencil(m_depth);
    // Phase A pre-emit: RG dispatch will call SetRenderTargetToHdr on the
    // GBufferPass primary CL — this transitions HDR (any state) → RT and
    // clears it inside this pass's CL. Without this, the SetRenderTargetsAndHdr
    // call inside Phase B's Execute() would race with downstream passes'
    // Phase A tracker mutations (LightingPass's HdrSceneColorPreserve sets
    // tracker→RT in Phase A on a *later* pass's CL, and GBufferPass's later
    // Phase B check then sees tracker==RT and skips emitting its own barrier
    // — but its CL submits FIRST and the GPU is still in last-frame's SR
    // state at that moment).
    //
    // Net effect: HDR clear + state transition are guaranteed to be in the
    // GBufferPass CL itself; the SetRenderTargetsAndHdr bind in Execute()
    // then overrides the 1-RT bind with the full 6-RT bind (clear is
    // already done, no need to re-clear).
    b.SetColorTarget(RG::BuiltinTexture::HdrSceneColor);
}

void GBufferPass::ReloadShaders(IGraphicsDevice& gfx)
{
    // Drop the compile cache + the PSO map; the next GetOrCreate hits
    // ShaderLibrary's fresh-compile path. Default-permutation PSO is rebuilt
    // here so it's warm by the next Execute; dynamic-material PSOs come back
    // online lazily as the frame's draws hit them.
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
    if (!m_psoCache.GetOrCreate(BuildPSODesc({})))
        LOG_ERROR("GBufferPass: PSO rebuild after shader reload failed");
}

void GBufferPass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS, RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    m_shaderLib.Register(ShaderID::GBuffer_PS, RHI::ShaderStage::PS, "GBuffer.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    if (!m_psoCache.GetOrCreate(BuildPSODesc({})))
        LOG_ERROR("GBufferPass: default PSO creation failed");
    else
        LOG_INFO("GBufferPass: default PSO ready");

    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t white = 0xFFFFFFFFu;
        RHI::SubresourceData init{ &white, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultWhite, &init))
        {
            m_defaultWhiteGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultWhite);
            if (!m_defaultWhiteGpuHandle)
                LOG_ERROR("GBufferPass: GetTextureSRVGpuHandle returned 0 for default white");
        }
        else
            LOG_ERROR("GBufferPass: failed to create default white texture");
    }

    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t flatNormal = 0xFFFF8080u;
        RHI::SubresourceData init{ &flatNormal, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultFlatNormal, &init))
        {
            m_defaultFlatNormalGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultFlatNormal);
            if (!m_defaultFlatNormalGpuHandle)
                LOG_ERROR("GBufferPass: GetTextureSRVGpuHandle returned 0 for flat-normal");
        }
        else
            LOG_ERROR("GBufferPass: failed to create flat-normal texture");
    }

    {
        // Anisotropic + negative mip bias: TAA integrates extra high-frequency
        // sub-pixel detail across frames, so biasing the sampler to fetch from
        // a sharper mip gives TAA something to accumulate. -0.5 is a safe
        // default (modern engines ship -0.5 to -1.0 when TAA is always-on);
        // going below -1.5 starts to alias faster than TAA can integrate.
        // Anisotropy 8 restores distant grazing-angle detail that trilinear
        // collapses, which is exactly where sub-pixel geometry (fences,
        // roof tiles, foliage) lives.
        RHI::SamplerDesc sd;
        sd.filter         = RHI::Filter::ANISOTROPIC;
        sd.max_anisotropy = 8;
        sd.mip_lod_bias   = -1.0f;   // native-res + always-on TAA → aggressive bias
        if (!gfx.CreateSampler(sd, m_linearSamplerIdx))
            LOG_ERROR("GBufferPass: failed to create linear sampler");
    }
}

// ---------------------------------------------------------------------------
PSODesc GBufferPass::BuildPSODesc(PermutationKey perm, uint32_t customPSID) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::GBuffer_VS;
    // Route custom shaders through the same PSO key — PSOCache hashes psID
    // as a uint32_t so dynamic IDs (> ShaderID::Count) coexist with the
    // static enum values without collision.
    desc.psID        = (customPSID != 0)
                        ? static_cast<ShaderID>(customPSID)
                        : ShaderID::GBuffer_PS;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cull_mode         = perm.Has(PermutationKey::DOUBLE_SIDED)
                                ? RHI::CullMode::NONE
                                : RHI::CullMode::BACK;
    desc.rs.depth_clip_enable = true;
    desc.dss.depth_enable     = true;
    desc.dss.depth_write_mask = RHI::DepthWriteMask::ALL;
    desc.dss.depth_func       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z
    // Stencil: write shader type ID per-pixel (ref set via OMSetStencilRef per draw).
    desc.dss.stencil_enable    = true;
    desc.dss.stencil_write_mask = 0xFF;
    desc.dss.stencil_read_mask  = 0xFF;
    desc.dss.front_face.stencil_pass_op = RHI::StencilOp::REPLACE;
    desc.dss.front_face.stencil_func    = RHI::ComparisonFunc::ALWAYS;
    desc.dss.back_face = desc.dss.front_face;

    for (int i = 0; i < 6; ++i)
        desc.bs.render_target[i].render_target_write_mask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R11G11B10_FLOAT;        // albedo (baseColor only — emissive split out to RT5)
    desc.rtvFormats[1] = RHI::Format::R16G16B16A16_FLOAT;     // normal
    desc.rtvFormats[2] = RHI::Format::R8G8B8A8_UNORM;         // surface
    desc.rtvFormats[3] = RHI::Format::R16G16_FLOAT;           // velocity
    desc.rtvFormats[4] = RHI::Format::R16G16B16A16_FLOAT;     // extra (shading-model scratch: SSS thickness, clearcoat, …)
    desc.rtvFormats[5] = RHI::Format::R16G16B16A16_FLOAT;     // HdrSceneColor (Unreal-style direct emissive write)
    desc.rtvCount      = 6;
    desc.dsvFormat     = RHI::Format::D24_UNORM_S8_UINT;
    return desc;
}

// ---------------------------------------------------------------------------
void GBufferPass::BindGlobals(RHI::CommandList cl, uint64_t bindlessHandle) const
{
    cl.BindDescriptorHeaps();
    cl.BindBufferSRVByName(kInstanceBufSlot, "InstanceBuffer");
    cl.BindBufferSRVByName(kMeshDescSlot,    "MeshDescriptors");
    if (bindlessHandle)
        cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
    cl.BindCBByName(0, "PerView");

    const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer("MaterialBuffer");
    if (matBuf && matBuf->IsValid())
        cl.BindBufferSRV(0, *matBuf);

    if (m_linearSamplerIdx >= 0)
        cl.BindSampler(0, m_linearSamplerIdx);

    if (m_defaultWhiteGpuHandle)
    {
        cl.BindDescriptorTableHandle(11, m_defaultWhiteGpuHandle);
        cl.BindDescriptorTableHandle(12, m_defaultWhiteGpuHandle);
    }
    if (m_defaultFlatNormalGpuHandle)
        cl.BindDescriptorTableHandle(13, m_defaultFlatNormalGpuHandle);

    // Bind bindless texture table (all loaded textures indexed by handle_id).
    auto& dx12 = static_cast<GraphicsDX12&>(*cl.gfx);
    D3D12_GPU_DESCRIPTOR_HANDLE texTable = dx12.GetBindlessTextureTableHandle();
    if (texTable.ptr)
        cl.BindDescriptorTableHandle(27, texTable.ptr); // kBindlessTexSlot = 27
}

// ---------------------------------------------------------------------------
// Records draws[begin..end) onto cl.
// Called on a TaskSystem worker thread.  Each invocation owns a distinct CL.
// Shared read-only state: m_psoCache (internally mutex-protected),
//   m_defaultWhiteGpuHandle, m_defaultFlatNormalGpuHandle, m_linearSamplerIdx.
// ---------------------------------------------------------------------------
void GBufferPass::RecordChunk(RHI::CommandList cl, DrawList draws, size_t begin, size_t end)
{
    // SetRenderTargets is required on every CL even though the clear already
    // happened on the primary CL — D3D12 does not inherit RT state across CLs.
    // 6th RT (HdrSceneColor) is appended internally by SetRenderTargetsAndHdr.
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const RHI::PipelineState* activePSO   = nullptr;
    const uint64_t            bindlessHdl = cl.GetBindlessTableHandle();

    for (size_t di = begin; di < end; ++di)
    {
        const DrawPacket& dp = draws[di];

        const RHI::PipelineState* pso =
            m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID));
        if (!pso || !pso->IsValid()) continue;

        if (pso != activePSO)
        {
            cl.SetPipelineState(*pso);
            activePSO = pso;
            BindGlobals(cl, bindlessHdl);
        }

        const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor  : m_defaultWhiteGpuHandle;
        const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap : m_defaultWhiteGpuHandle;
        const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap  : m_defaultFlatNormalGpuHandle;
        if (bc) cl.BindDescriptorTableHandle(11, bc);
        if (sm) cl.BindDescriptorTableHandle(12, sm);
        if (nm) cl.BindDescriptorTableHandle(13, nm);

        // Set stencil ref per-draw: 1=PBR, 2=NPR (written by stencil REPLACE op)
        static_cast<GraphicsDX12&>(*cl.gfx).GetNativeCommandList(cl)
            ->OMSetStencilRef(dp.stencilRef);

        // Phase E: bind the per-material custom CBV (b8 space0) when the
        // material ran a packed custom shader this frame. Zero means either
        // the standard GBuffer PS is in use or the custom PS has no cbuffer.
        if (dp.customCbvVA)
            static_cast<GraphicsDX12&>(*cl.gfx).BindCustomMaterialCBV(dp.customCbvVA, cl);
        // Phase F: bind the per-material custom texture table (t0-t3 space3).
        if (dp.customTexTable)
            static_cast<GraphicsDX12&>(*cl.gfx).BindCustomMaterialTextureTable(dp.customTexTable, cl);

        // Write all 4 PVF root constants atomically. Splitting into two
        // calls (3+1) is legal per D3D12 spec but tripped a driver corner
        // case during the P1-P6 investigation where the 4th constant failed
        // to propagate — a single SetGraphicsRoot32BitConstants is safer
        // and has no measurable cost.
        {
            uint32_t constants[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                dp.prevPosElementBase
            };
            static_cast<GraphicsDX12&>(*cl.gfx).GetNativeCommandList(cl)
                ->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        }
        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }
}

// ---------------------------------------------------------------------------
RHI::CommandList GBufferPass::Execute(RHI::CommandList cl)
{
    // Primary CL (main thread): clear targets only, zero draws.
    // Chunk CLs (worker threads): SetRenderTargets + draw subset.
    //
    // RG's Phase A already emitted the HDR transition (any state → RT) and
    // ClearRenderTargetView for HDR into THIS primary CL via the
    // BuiltinTexture::HdrSceneColor color-target declaration in Setup().
    // SetRenderTargetsAndHdr below replaces that 1-RT bind with the full
    // 6-RT bind (5 GBuffer RTs + HDR appended). HDR clear is NOT re-done.
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    const float black[4]    = { 0.f, 0.f, 0.f, 1.f };
    cl.ClearRenderTarget(m_albedo,   black);
    cl.ClearRenderTarget(m_normal,   black);
    cl.ClearRenderTarget(m_surface,  black);
    cl.ClearRenderTarget(m_velocity, black);
    cl.ClearRenderTarget(m_emissive, black);
    cl.ClearDepthStencil(m_depth);

    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    if (draws.empty() && !m_indirectArgBuffer) return cl;

    // ---- ExecuteIndirect path (GPU-culled, per-PSO group) -------------------
    if (m_indirectArgBuffer && !m_indirectGroups.empty())
    {
        auto& dx12 = static_cast<GraphicsDX12&>(*cl.gfx);
        const uint64_t bindlessHdl = cl.GetBindlessTableHandle();

        cl.SetViewport();
        cl.SetScissorRect();
        cl.SetPrimitiveTopology();

        for (const auto& group : m_indirectGroups)
        {
            if (group.cmdCount == 0) continue;

            const RHI::PipelineState* pso =
                m_psoCache.GetOrCreate(BuildPSODesc(group.perm, group.customPSID));
            if (!pso || !pso->IsValid()) continue;

            cl.SetPipelineState(*pso);
            BindGlobals(cl, bindlessHdl);

            dx12.GetNativeCommandList(cl)->OMSetStencilRef(group.stencilRef);

            // Phase E: bind the group's per-material CBV once before
            // ExecuteIndirect. All commands in a group share the same
            // material (and thus the same packed CBV VA), so one bind covers
            // the entire cmdCount.
            if (group.customCbvVA)
                dx12.BindCustomMaterialCBV(group.customCbvVA, cl);
            // Phase F: same for the custom texture table.
            if (group.customTexTable)
                dx12.BindCustomMaterialTextureTable(group.customTexTable, cl);

            cl.gfx->ExecuteIndirectDraw(
                *m_indirectArgBuffer,
                group.argOffset,
                group.cmdCount,
                nullptr, 0,  // no GPU-side count buffer per group (CPU provides exact count)
                cl);
        }
        return cl;
    }

    const int drawCount = static_cast<int>(draws.size());
    const int numChunks = std::clamp(drawCount / kMinDrawsPerChunk, 1, kMaxChunks);

    // ---- Single-CL fast path -----------------------------------------------
    // No TaskSystem overhead for small draw lists.
    if (numChunks == 1)
    {
        RecordChunk(cl, draws, 0, draws.size());
        return cl;
    }

    // ---- Parallel path ---------------------------------------------------------
    // Each worker independently calls BeginCommandList() on its own thread.
    // This means allocator Reset + CL Reset + SetDescriptorHeaps all happen in
    // parallel, not sequentially on the main thread.
    //
    // Thread-safety guarantees:
    //   BeginCommandList: mutex protects counter++ and push_back; per-CL setup
    //   operates on disjoint pool entries → no contention.
    //   m_commandLists is pre-reserved(64) so push_back never reallocates while
    //   other threads read existing entries via GetPoolEntry.
    //   m_psoCache: internally mutex-protected.
    //   draws / RenderContext: read-only → safe from all threads.

    IGraphicsDevice*   gfxPtr = cl.gfx;
    RG::RenderContext* ctxPtr = cl.ctx;
    const size_t chunkSize    = (draws.size() + numChunks - 1) / numChunks;

    // Workers write their finished CL handles here; main thread reads after wait.
    RHI::CommandList chunkCLs[kMaxChunks]{};
    std::latch done(numChunks);

    for (int i = 0; i < numChunks; ++i)
    {
        const size_t begin = static_cast<size_t>(i) * chunkSize;
        const size_t end   = std::min(begin + chunkSize, draws.size());

        TaskSystem::Get().Push(
            [this, gfxPtr, ctxPtr, draws, begin, end, i, &chunkCLs, &done]() mutable
            {
                // Open the CL on this worker thread: Reset + SetDescriptorHeaps
                // happen here, in parallel with the other workers.
                RHI::CommandList chunkCL = gfxPtr->BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
                chunkCL.gfx = gfxPtr;
                chunkCL.ctx = ctxPtr;

                RecordChunk(chunkCL, draws, begin, end);

                // Publish finished handle before counting down (latch is the fence).
                chunkCLs[i] = chunkCL;
                done.count_down();
            },
            TaskSystem::TaskPriority::High);
    }

    // Main thread is free to do other work here if needed in the future.
    done.wait();

    // Return the last chunk CL so LightingPass continues recording on it.
    // EndFrame submits in allocation order, which is:
    //   primaryCL (clear) → chunkCL[0] → chunkCL[1] → … → chunkCL[N-1]
    // GPU executes these sequentially on the GRAPHICS queue.
    return chunkCLs[numChunks - 1];
}
