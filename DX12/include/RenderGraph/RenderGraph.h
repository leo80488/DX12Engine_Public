#pragma once

// RenderGraph — transient resource manager, barrier scheduler, and pass executor.
//
// Responsibility boundary (architecture contract):
//   - Allocates / destroys GPU textures via IGraphicsDevice::CreateTexture.
//   - Computes and emits resource barriers before each pass via IGraphicsDevice::PushBarrier.
//   - Builds a RenderContext per frame and hands it to each pass Execute.
//   - Does NOT contain draw call logic or scene data.
//   - No DX12 types (ID3D12*, D3D12_*, DXGI_FORMAT) appear in this header.

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "Graphics/GraphicsStruct.h"
#include "RenderGraph/RGTypes.h"
#include "RenderGraph/RenderContext.h"

struct DrawPacket;    // full type in Graphics/RenderTypes.h

class IGraphicsDevice;
class ShaderLibrary;
class PSOCache;

namespace RG
{
    enum class BuiltinTexture
    {
        SwapChainColor,
        HdrSceneColor,           // Auto-bind + CLEAR (legacy: pass is first HDR writer)
        HdrSceneColorPreserve,   // Auto-bind, NO clear — pass is a downstream writer
                                 // (e.g. LightingPass, additive on top of GBufferPass's
                                 // emissive seed). Same state transitions as HdrSceneColor;
                                 // only the clear is skipped.
        None,   // Pass handles its own render target setup inside Execute.
    };

    // Forward declarations
    class RenderPass;
    class RenderGraphBuilder;

    // =========================================================================
    class RenderGraph
    {
    public:
        ~RenderGraph() = default;

        // ---- Class-based pass registration (backward compat) ----------------
        void AddPass(std::unique_ptr<RenderPass> pass);

        // ---- Lambda-based pass registration (6-layer architecture) ----------
        void AddPass(const char* name,
                     std::function<void(RenderGraphBuilder&)>   setupFn,
                     std::function<void(RHI::CommandList&)>     executeFn);

        // ---- Graph-scope texture declaration --------------------------------
        // Handle is stable across Compile() calls; safe to capture in lambdas.
        RGTextureHandle CreateTexture(const char* name, const RGTextureDesc& desc);

        // Calls Setup() on all passes, creates physical GPU textures via gfx,
        // then calls Init() on all passes.
        void Compile(IGraphicsDevice& gfx);

        // Force a recompile on the next Execute() call (recreates all virtual textures).
        // Call when the window is resized so graph-managed textures match the new dimensions.
        void MarkDirty() { m_compiled = false; }

        // Allocates one command list per pass (via IGraphicsDevice::BeginCommandList),
        // emits per-pass barriers, records draws, and chains execution dependencies
        // so passes run in declaration order on the GPU.
        // Returns the last pass command list (callers can declare further dependencies
        // on it, e.g. to ensure the primary CL executes after the final pass).
        // Returns an invalid CommandList if the graph has no passes.
        RHI::CommandList Execute(IGraphicsDevice& gfx, const float clearColor[4]);

        // Hot-reload entry point. Delegates to RenderPass::ReloadShaders on
        // every registered pass. Caller is responsible for ensuring the GPU
        // is idle (no in-flight commands referencing the old PSOs) — the
        // App-level wiring achieves this by calling between EndFrame and the
        // next BeginFrame, after the device has waited on the previous frame.
        void ReloadShaders(IGraphicsDevice& gfx);

        // Register a per-frame constant buffer; must be called before Execute.
        void BindConstantBuffer(const char* name, const RHI::GPUBuffer& buffer);

        // Register a GPU buffer for root SRV binding (InstanceBuffer, MeshDescriptors etc).
        // Must be called before Execute. Accessed in passes via RenderContext::GetBuffer.
        void BindBuffer(const char* name, const RHI::GPUBuffer& buffer);

        // Set the bindless g_Buffers[] descriptor table GPU handle for this frame.
        void SetBindlessTableHandle(uint64_t gpuHandle) { m_bindlessTableHandle = gpuHandle; }

        // Supply the frame's sorted draw list so passes can iterate DrawPackets.
        // Must be called before Execute.  Pointer must remain valid during Execute.
        void SetDrawList(const std::vector<DrawPacket>* packets);

        // ---- Setup-phase API (invoked by RenderGraphBuilder during Compile) --
        RGTextureHandle DeclareTexture(const char* name, const RGTextureDesc& desc);
        RGTextureHandle LookupTexture(const char* name) const;
        void            RecordWrite(RGTextureHandle h, bool isDepth);
        void            RecordRead(RGTextureHandle h);
        void            RecordUAV(RGTextureHandle h);
        void            SetBuiltinTarget(BuiltinTexture t);

        // Expose the physical GPU texture for a graph-managed handle.
        // Returns nullptr if the handle is invalid or the graph has not been compiled.
        // Use this to obtain SRV/UAV GPU handles for passes running outside the graph.
        const RHI::Texture* GetPhysicalTexture(RGTextureHandle h) const;

        // ---- State export/import for out-of-graph passes -----------------------
        // After Execute(), out-of-graph passes (HiZ, post-processing) can query
        // what state a texture was left in, and update it after they transition it.
        // This eliminates hardcoded state assumptions at graph boundaries.
        RHI::ResourceState GetTextureState(RGTextureHandle h) const;
        void               SetTextureState(RGTextureHandle h, RHI::ResourceState state);

    private:
        // Graph-scope texture spec (set by CreateTexture, survives Compile resets).
        struct GraphTexture { std::string name; RGTextureDesc desc; };

        // Physical backing for a virtual texture declared by a pass.
        struct VirtualTexture
        {
            std::string        name;
            RGTextureDesc      desc;
            RHI::Texture       texture;
            RHI::ResourceState currentState = RHI::ResourceState::UNDEFINED;
            bool hasRTV = false;
            bool hasDSV = false;
            bool hasSRV = false;
            bool hasUAV = false;
        };

        struct PassNode
        {
            std::unique_ptr<RenderPass>  pass;
            BuiltinTexture               colorTarget = BuiltinTexture::None;
            std::vector<RGTextureHandle> rtHandles;
            std::vector<RGTextureHandle> dsHandles;
            std::vector<RGTextureHandle> srvHandles;
            std::vector<RGTextureHandle> uavHandles;
            bool                         initialized = false;
        };

        bool                                                    m_compiled = false;
        const std::vector<DrawPacket>*                          m_drawList = nullptr;
        uint64_t                                                m_bindlessTableHandle = 0;
        std::vector<GraphTexture>                               m_graphTextures;
        std::vector<PassNode>                                   m_passes;
        std::vector<VirtualTexture>                             m_textures;
        std::unordered_map<std::string, uint32_t>               m_nameToIndex;
        std::unordered_map<std::string, const RHI::GPUBuffer*>  m_cbBindings;
        std::unordered_map<std::string, const RHI::GPUBuffer*>  m_bufferBindings;

        PassNode* m_currentSetupNode = nullptr;

        void CreatePhysicalTextures(IGraphicsDevice& gfx);
        void EmitBarriersBeforePass(IGraphicsDevice& gfx,
                                    RHI::CommandList  cmd,
                                    PassNode&         node);
    };

    // =========================================================================
    // RenderPass — base class for atomic rendering steps.
    // =========================================================================
    class RenderPass
    {
    public:
        virtual ~RenderPass() = default;
        virtual const char* GetName() const = 0;
        virtual void Setup(RenderGraphBuilder& builder) = 0;
        virtual void Init(IGraphicsDevice& gfx) = 0;
        // Called serially in RenderGraph::Compile after physical textures are
        // (re)created.  Override to (re)create pass-owned GPU resources that must
        // stay in sync with the current render dimensions, before parallel Execute.
        virtual void OnCompile(IGraphicsDevice& /*gfx*/) {}
        // Records draw calls onto @p cl.  cl.GetDevice() / cl.GetContext() provide
        // access to the backend and per-frame data.  Returns @p cl.
        virtual RHI::CommandList Execute(RHI::CommandList cl) = 0;

        // Hot-reload accessor pattern. Override these to expose any owned
        // ShaderLibrary / PSOCache instances; the default ReloadShaders
        // implementation below uses them to clear caches uniformly. This is
        // the *low-cost* path — a pass opts in with a 2-line override instead
        // of writing a full ReloadShaders body.
        virtual ShaderLibrary*           GetReloadableShaderLibrary() { return nullptr; }
        virtual std::vector<PSOCache*>   GetReloadablePSOCaches()     { return {}; }

        // Hot-reload hook. Invoked by RenderGraph::ReloadShaders when an
        // .hlsl/.hlsli file changes on disk.
        //
        // Default behaviour: walks GetReloadableShaderLibrary /
        // GetReloadablePSOCaches and Clears() them. The next frame's draw
        // calls hit empty caches → ShaderLibrary recompiles via DXC,
        // PSOCache rebuilds the PSO. Causes a brief first-draw stutter
        // post-reload but is correct without any per-pass code beyond the
        // accessors.
        //
        // Override directly when a pass needs more (e.g. eager warm-up of
        // specific permutations to avoid the stutter, refreshing reflection
        // pointers, recreating descriptor heaps). GBufferPass / LightingPass
        // / ShadowPass override this directly to warm up their default-perm
        // PSOs.
        virtual void ReloadShaders(IGraphicsDevice& /*gfx*/);
    };

    // =========================================================================
    // RenderGraphBuilder — passed to RenderPass::Setup.
    // =========================================================================
    class RenderGraphBuilder
    {
    public:
        explicit RenderGraphBuilder(RenderGraph& graph) : m_graph(graph) {}

        RGTextureHandle DeclareTexture(const char* name, const RGTextureDesc& desc)
        {
            return m_graph.DeclareTexture(name, desc);
        }
        RGTextureHandle GetTexture(const char* name) const
        {
            return m_graph.LookupTexture(name);
        }
        void WriteRenderTarget(RGTextureHandle h) { m_graph.RecordWrite(h, false); }
        void WriteDepthStencil(RGTextureHandle h) { m_graph.RecordWrite(h, true);  }
        void ReadSRV(RGTextureHandle h)            { m_graph.RecordRead(h);         }
        // Declares @p h will be bound as a UAV by the current pass. Causes the
        // graph to (a) create the physical texture with UNORDERED_ACCESS and
        // (b) transition it to UNORDERED_ACCESS before the pass runs.
        void WriteUAV(RGTextureHandle h)           { m_graph.RecordUAV(h);          }
        void SetColorTarget(BuiltinTexture t)      { m_graph.SetBuiltinTarget(t);   }

    private:
        RenderGraph& m_graph;
    };

    // Alias — preferred name in the 6-layer architecture; backward-compatible.
    using RGPassBuilder = RenderGraphBuilder;

} // namespace RG
