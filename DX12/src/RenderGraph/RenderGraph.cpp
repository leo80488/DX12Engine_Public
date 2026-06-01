#include "RenderGraph/RenderGraph.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "System/Log.h"
#include "System/TaskSystem.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>

namespace RG
{
    // =========================================================================
    // LambdaRenderPass
    // =========================================================================
    class LambdaRenderPass : public RenderPass
    {
    public:
        LambdaRenderPass(std::string name,
                         std::function<void(RenderGraphBuilder&)>  setupFn,
                         std::function<void(RHI::CommandList&)>    executeFn)
            : m_name(std::move(name))
            , m_setupFn(std::move(setupFn))
            , m_executeFn(std::move(executeFn))
        {}

        const char* GetName() const override { return m_name.c_str(); }
        void Setup(RenderGraphBuilder& b) override { if (m_setupFn) m_setupFn(b); }
        void Init(IGraphicsDevice&)       override {}

        RHI::CommandList Execute(RHI::CommandList cl) override
        {
            if (m_executeFn) m_executeFn(cl);
            return cl;
        }

    private:
        std::string                               m_name;
        std::function<void(RenderGraphBuilder&)>  m_setupFn;
        std::function<void(RHI::CommandList&)>    m_executeFn;
    };

    // -------------------------------------------------------------------------
    void RenderGraph::AddPass(std::unique_ptr<RenderPass> pass)
    {
        if (!pass) return;
        PassNode node;
        node.pass = std::move(pass);
        m_passes.push_back(std::move(node));
        m_compiled = false;
    }

    void RenderGraph::AddPass(const char* name,
                              std::function<void(RenderGraphBuilder&)>  setupFn,
                              std::function<void(RHI::CommandList&)>    executeFn)
    {
        AddPass(std::make_unique<LambdaRenderPass>(
            name ? name : "", std::move(setupFn), std::move(executeFn)));
    }

    RGTextureHandle RenderGraph::CreateTexture(const char* name, const RGTextureDesc& desc)
    {
        assert(name);
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_graphTextures.size()); ++i)
            if (m_graphTextures[i].name == name) return { i };

        uint32_t idx = static_cast<uint32_t>(m_graphTextures.size());
        m_graphTextures.push_back({ name, desc });
        m_compiled = false;
        return { idx };
    }

    void RenderGraph::BindConstantBuffer(const char* name, const RHI::GPUBuffer& buffer)
    {
        m_cbBindings[name] = &buffer;
    }

    void RenderGraph::BindBuffer(const char* name, const RHI::GPUBuffer& buffer)
    {
        m_bufferBindings[name] = &buffer;
    }

    void RenderGraph::SetDrawList(const std::vector<DrawPacket>* packets)
    {
        m_drawList = packets;
    }

    void RenderGraph::SetExternalWait(const char* passName, RHI::CommandList depCL)
    {
        if (!passName) return;
        m_externalWaits[passName] = depCL;
    }

    // ---- Setup-phase API ---------------------------------------------------

    RGTextureHandle RenderGraph::DeclareTexture(const char* name, const RGTextureDesc& desc)
    {
        assert(name);
        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end()) return { it->second };

        uint32_t idx = static_cast<uint32_t>(m_textures.size());
        VirtualTexture vt;
        vt.name = name;
        vt.desc = desc;
        m_textures.push_back(std::move(vt));
        m_nameToIndex[name] = idx;
        return { idx };
    }

    RGTextureHandle RenderGraph::LookupTexture(const char* name) const
    {
        auto it = m_nameToIndex.find(name);
        if (it == m_nameToIndex.end()) return {};
        return { it->second };
    }

    const RHI::Texture* RenderGraph::GetPhysicalTexture(RGTextureHandle h) const
    {
        if (!h.IsValid() || h.id >= m_textures.size()) return nullptr;
        const VirtualTexture& vt = m_textures[h.id];
        return vt.texture.IsValid() ? &vt.texture : nullptr;
    }

    RHI::ResourceState RenderGraph::GetTextureState(RGTextureHandle h) const
    {
        if (!h.IsValid() || h.id >= m_textures.size())
            return RHI::ResourceState::UNDEFINED;
        return m_textures[h.id].currentState;
    }

    void RenderGraph::SetTextureState(RGTextureHandle h, RHI::ResourceState state)
    {
        if (h.IsValid() && h.id < m_textures.size())
            m_textures[h.id].currentState = state;
    }

    void RenderGraph::RecordWrite(RGTextureHandle h, bool isDepth)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        if (isDepth) m_currentSetupNode->dsHandles.push_back(h);
        else         m_currentSetupNode->rtHandles.push_back(h);
        if (h.id < m_textures.size())
        {
            if (isDepth) m_textures[h.id].hasDSV = true;
            else         m_textures[h.id].hasRTV = true;
        }
    }

    void RenderGraph::RecordRead(RGTextureHandle h)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        m_currentSetupNode->srvHandles.push_back(h);
        if (h.id < m_textures.size()) m_textures[h.id].hasSRV = true;
    }

    void RenderGraph::RecordUAV(RGTextureHandle h)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        m_currentSetupNode->uavHandles.push_back(h);
        if (h.id < m_textures.size()) m_textures[h.id].hasUAV = true;
    }

    void RenderGraph::SetBuiltinTarget(BuiltinTexture t)
    {
        if (m_currentSetupNode) m_currentSetupNode->colorTarget = t;
    }

    // ---- Compile -----------------------------------------------------------

    void RenderGraph::Compile(IGraphicsDevice& gfx)
    {
        // Free physical textures from any previous compile.
        for (auto& vt : m_textures)
            if (vt.texture.IsValid()) gfx.DestroyTexture(vt.texture);

        m_textures.clear();
        m_nameToIndex.clear();

        // Pre-populate from graph-scope textures (handles from CreateTexture are stable).
        for (auto& gt : m_graphTextures)
        {
            uint32_t idx = static_cast<uint32_t>(m_textures.size());
            VirtualTexture vt;
            vt.name = gt.name;
            vt.desc = gt.desc;
            m_textures.push_back(std::move(vt));
            m_nameToIndex[gt.name] = idx;
        }

        // Run Setup on each pass to collect read/write declarations.
        for (auto& node : m_passes)
        {
            node.rtHandles.clear();
            node.dsHandles.clear();
            node.srvHandles.clear();
            node.uavHandles.clear();
            node.colorTarget = BuiltinTexture::None;

            m_currentSetupNode = &node;
            RenderGraphBuilder builder(*this);
            node.pass->Setup(builder);
            m_currentSetupNode = nullptr;
        }

        // Propagate per-pass UAV/SRV declarations into the per-texture flags so
        // the physical creation step picks up bind flags for textures written
        // as UAV from any pass (e.g. in-place GBuffer decal compositing).
        for (const auto& node : m_passes)
        {
            for (const auto& h : node.uavHandles)
                if (h.IsValid() && h.id < m_textures.size())
                    m_textures[h.id].hasUAV = true;
            for (const auto& h : node.srvHandles)
                if (h.IsValid() && h.id < m_textures.size())
                    m_textures[h.id].hasSRV = true;
        }
        // Also honour isUAV / isSRV declared at CreateTexture time. isSRV is for
        // textures read as an SRV from OUTSIDE the graph (e.g. GBuffer velocity
        // read by TAA/XeGTAO/SSR via direct SRV handles) — no in-graph pass
        // declares the read, so without this the physical resource would be
        // created without SHADER_RESOURCE and GetTextureSRVGpuHandle returns 0.
        for (auto& vt : m_textures)
        {
            if (vt.desc.isUAV) vt.hasUAV = true;
            if (vt.desc.isSRV) vt.hasSRV = true;
        }

        CreatePhysicalTextures(gfx);

        for (auto& node : m_passes)
        {
            if (!node.initialized)
            {
                node.pass->Init(gfx);
                node.initialized = true;
            }
        }

        // Give every pass a chance to (re)create its own GPU resources in the
        // serial phase — must happen BEFORE the parallel Execute phase to avoid
        // data races on the descriptor heap allocator.
        for (auto& node : m_passes)
            node.pass->OnCompile(gfx);

        m_compiled = true;
        LOG_INFO("RenderGraph: compiled %zu passes, %zu virtual textures",
                 m_passes.size(), m_textures.size());
    }

    void RenderGraph::CreatePhysicalTextures(IGraphicsDevice& gfx)
    {
        const uint32_t W = gfx.GetRenderWidth();
        const uint32_t H = gfx.GetRenderHeight();

        for (auto& vt : m_textures)
        {
            if (vt.texture.IsValid()) continue;

            RHI::TextureDesc tdesc;
            tdesc.format = vt.desc.format;
            tdesc.width  = vt.desc.width  ? vt.desc.width  : W;
            tdesc.height = vt.desc.height ? vt.desc.height : H;
            tdesc.usage  = RHI::Usage::DEFAULT;

            RHI::BindFlag flags = RHI::BindFlag::NONE;
            if (vt.hasRTV) flags |= RHI::BindFlag::RENDER_TARGET;
            if (vt.hasDSV) flags |= RHI::BindFlag::DEPTH_STENCIL;
            if (vt.hasSRV) flags |= RHI::BindFlag::SHADER_RESOURCE;
            if (vt.hasUAV) flags |= RHI::BindFlag::UNORDERED_ACCESS;
            tdesc.bind_flags = flags;

            if (vt.desc.isDepth)
            {
                tdesc.layout = RHI::ResourceState::DEPTHSTENCIL;
                tdesc.clear  = RHI::ClearValue::DepthStencil(0.0f, 0);  // reversed Z: far = 0
                vt.currentState = RHI::ResourceState::DEPTHSTENCIL;
            }
            else
            {
                tdesc.layout = RHI::ResourceState::RENDERTARGET;
                tdesc.clear  = RHI::ClearValue::Color(0, 0, 0, 1);
                vt.currentState = RHI::ResourceState::RENDERTARGET;
            }

            // Forward the virtual-texture name to the resource so D3D12
            // validation messages identify it instead of "Unnamed".
            tdesc.debug_name = vt.name.c_str();

            if (!gfx.CreateTexture(tdesc, vt.texture))
            {
                LOG_ERROR("RenderGraph: CreateTexture failed for '%s'", vt.name.c_str());
            }
        }
    }

    // ---- Execute -----------------------------------------------------------

    RHI::CommandList RenderGraph::Execute(IGraphicsDevice& gfx, const float clearColor[4])
    {
        if (!m_compiled) Compile(gfx);

        const size_t passCount = m_passes.size();
        if (passCount == 0) return {};

        // Build the shared read-only context (textures, CBs, draw list).
        // Passes access this concurrently during Phase 2 — no writes occur after setup.
        RenderContext ctx;
        ctx.SetDimensions(gfx.GetRenderWidth(), gfx.GetRenderHeight());
        ctx.SetDrawList(m_drawList);

        for (auto& [name, buf] : m_cbBindings)
            if (buf) ctx.SetCB(name.c_str(), *buf);

        for (auto& [name, buf] : m_bufferBindings)
            if (buf) ctx.SetBuffer(name.c_str(), *buf);

        ctx.SetBindlessTableHandle(m_bindlessTableHandle);

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_textures.size()); ++i)
            ctx.SetTexture({ i }, m_textures[i].texture);

        // ---- Two-phase recording: setup serial, Execute parallel --------------
        // Phase A (serial, main thread):
        //   Open each pass's CL, set its render target, emit resource barriers,
        //   wire the GPU dependency chain, and open the GPU profiler region.
        //   Barriers can't run in parallel because they share the texture state
        //   tracker (m_textures[].currentState) — two passes racing on the same
        //   texture would miss a transition.
        // Phase B (parallel, worker threads):
        //   Each worker calls m_passes[i].pass->Execute(passCLs[i]). The shared
        //   RenderContext is read-only per the class contract, and each pass
        //   writes only to its own CommandList. GBufferPass still chunks its
        //   draws across additional workers internally — that keeps working.
        // Phase C (serial): close profiler regions (sequential CL writes after
        //   Execute finished).
        struct PassRecord {
            RHI::CommandList cl;
            uint32_t         profilerRegion;
        };
        std::vector<PassRecord> recs(passCount);
        RHI::CommandList prevCL{};

        // ---- Phase A: allocate CLs + barriers (serial) ----
        for (size_t i = 0; i < passCount; ++i)
        {
            auto& node = m_passes[i];
            PassRecord& r = recs[i];
            r.cl = gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
            r.profilerRegion = ~0u;

            if (prevCL.IsValid())
                gfx.AddCommandListDependency(r.cl, prevCL);

            // External cross-queue wait (e.g. LightingPass ← DDGI compute CL).
            // Registered by the caller via SetExternalWait before Execute;
            // landing the wait here means GBuffer / Terrain / SkyIBL keep
            // running in parallel with the async producer while only the
            // dependent pass stalls.
            if (!m_externalWaits.empty())
            {
                auto it = m_externalWaits.find(m_passes[i].pass->GetName());
                if (it != m_externalWaits.end() && it->second.IsValid())
                    gfx.AddCommandListDependency(r.cl, it->second);
            }

            if (node.colorTarget == BuiltinTexture::HdrSceneColor)
                gfx.SetRenderTargetToHdr(clearColor, r.cl);
            else if (node.colorTarget == BuiltinTexture::HdrSceneColorPreserve)
                gfx.SetRenderTargetToHdrWithDepth(nullptr, r.cl);   // bind HDR, no clear
            else if (node.colorTarget == BuiltinTexture::SwapChainColor)
                gfx.SetRenderTargetToSwapChain(clearColor, r.cl);

            EmitBarriersBeforePass(gfx, r.cl, node);

            r.cl.gfx = &gfx;
            r.cl.ctx = &ctx;

            r.profilerRegion = gfx.BeginGPUTimestamp(
                r.cl, m_passes[i].pass->GetName());

            prevCL = r.cl;
        }

        // ---- Phase B: record each pass in parallel ----
        if (passCount >= 4)
        {
            TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(passCount),
                [&](uint32_t i) {
                    m_passes[i].pass->Execute(recs[i].cl);
                });
        }
        else
        {
            // Too few passes for task dispatch to be worth it — serial.
            for (size_t i = 0; i < passCount; ++i)
                m_passes[i].pass->Execute(recs[i].cl);
        }

        // ---- Phase C: close profiler regions (serial, CL writes) ----
        if (gfx.IsGPUProfilerEnabled())
        {
            for (size_t i = 0; i < passCount; ++i)
                gfx.EndGPUTimestamp(recs[i].cl, recs[i].profilerRegion);
        }

        // External waits are per-frame; clear so a stale dep from a previous
        // frame doesn't leak into the next graph run.
        m_externalWaits.clear();

        return recs[passCount - 1].cl;
    }

    // -------------------------------------------------------------------------
    // RenderPass::ReloadShaders default impl — walks the accessor virtuals.
    // Passes that need warm-up or extra state refresh override this directly.
    // -------------------------------------------------------------------------
    void RenderPass::ReloadShaders(IGraphicsDevice& /*gfx*/)
    {
        if (auto* lib = GetReloadableShaderLibrary())
            lib->ClearCaches();
        for (auto* cache : GetReloadablePSOCaches())
            if (cache) cache->Clear();
    }

    void RenderGraph::ReloadShaders(IGraphicsDevice& gfx)
    {
        // Each pass's override is the source of truth for what state needs
        // refreshing. Default RenderPass::ReloadShaders is a no-op, so passes
        // that haven't opted in are silently skipped — that's acceptable for
        // a v1 hot-reload (developer extends coverage as needed).
        for (auto& node : m_passes)
            node.pass->ReloadShaders(gfx);
        LOG_INFO("RenderGraph: shaders reloaded across %zu passes", m_passes.size());
    }

    void RenderGraph::EmitBarriersBeforePass(IGraphicsDevice& gfx,
                                              RHI::CommandList  cmd,
                                              PassNode&         node)
    {
        auto transition = [&](RGTextureHandle h, RHI::ResourceState targetState)
        {
            if (!h.IsValid() || h.id >= m_textures.size()) return;
            auto& vt = m_textures[h.id];
            if (!vt.texture.IsValid()) return;
            if (vt.currentState == targetState) return;

            gfx.PushBarrier(
                RHI::GPUBarrier::Image(&vt.texture, vt.currentState, targetState),
                cmd);
            vt.currentState = targetState;
        };

        for (auto& h : node.rtHandles)  transition(h, RHI::ResourceState::RENDERTARGET);
        for (auto& h : node.dsHandles)  transition(h, RHI::ResourceState::DEPTHSTENCIL);
        for (auto& h : node.uavHandles) transition(h, RHI::ResourceState::UNORDERED_ACCESS);
        for (auto& h : node.srvHandles)
        {
            const bool isDepthTex = (h.id < m_textures.size()) && m_textures[h.id].desc.isDepth;
            transition(h, isDepthTex ? RHI::ResourceState::DEPTH_READ_SRV
                                     : RHI::ResourceState::SHADER_RESOURCE);
        }
    }

} // namespace RG
