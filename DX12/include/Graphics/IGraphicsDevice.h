#pragma once

// IGraphicsDevice: platform-agnostic virtual graphics interface.
// All methods use RHI descriptor structs and resource handles from GraphicsStruct.h,
// keeping callers completely free of backend-specific types (DX12/Vulkan/etc.).
//
// Multi-threaded recording model:
//   BeginFrame()                          — CPU sync + opens the primary command list. Returns CommandList{0}.
//   BeginCommandList(queue)               — Opens an additional command list from the pool.
//                                           Safe to call from worker threads concurrently.
//   AddCommandListDependency(waiter, dep) — Declare that waiter must execute after dep on the GPU.
//                                           Same-queue: enforced by topological submission order.
//                                           Cross-queue: GPU fence Wait/Signal inserted by EndFrame.
//   All draw / bind / barrier / state calls take a CommandList parameter;
//   multiple threads may record into different CommandLists in parallel.
//   EndFrame()                            — Topological-sorts all active CLs by their declared
//                                           dependencies, inserts cross-queue fences where needed,
//                                           closes + submits all lists, then presents.

#include "Graphics/GraphicsStruct.h"
#include <cstdint>

class IGraphicsDevice
{
public:
    virtual ~IGraphicsDevice() = default;

    // =========================================================================
    // Frame lifecycle
    // =========================================================================

    /** Fence-wait for previous frame, advance frame index, open the primary
     *  command list (id 0). Returns a CommandList handle for the primary list.
     *  Pushes the swap-chain PRESENT→RENDER_TARGET barrier on that list. */
    virtual RHI::CommandList BeginFrame() = 0;

    /** Close + execute all active command lists, present, signal fence. */
    virtual void EndFrame() = 0;

    /** Signal the graphics queue fence and CPU-wait until all submitted GPU work completes.
     *  Use sparingly (editor operations only — stalls the CPU). */
    virtual void FlushAndWait() = 0;

    // =========================================================================
    // Command list management
    // =========================================================================

    /** Open an additional command list from the pool (thread-safe).
     *  Returns a CommandList handle that must be passed to all recording calls.
     *  Reuses allocators from the pool; lazily creates DX12 objects on first use. */
    virtual RHI::CommandList BeginCommandList(
        RHI::QUEUE_TYPE queue = RHI::QUEUE_TYPE::GRAPHICS) = 0;

    /** Declare a GPU execution dependency: @p waiter must run after @p dependency.
     *  - Same-queue pair  → enforced by topological submission order (no overhead).
     *  - Cross-queue pair → EndFrame inserts a GPU fence Wait/Signal at the boundary.
     *  Must be called after both command lists are opened and before EndFrame(). */
    virtual void AddCommandListDependency(RHI::CommandList waiter,
                                          RHI::CommandList dependency) = 0;

    // =========================================================================
    // Window / swap chain management
    // =========================================================================

    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual void SetFullscreen(bool fullscreen) = 0;

    /** Set the scene viewport size (ImGui panel size). 0,0 means use window size. */
    virtual void SetViewportSize(uint32_t width, uint32_t height) = 0;

    /** Bind the HDR offscreen render target and clear it.
     *  Records into @p cmd. Call after BeginFrame / BeginCommandList. */
    virtual void SetRenderTargetToHdr(const float clearColor[4],
                                      RHI::CommandList cmd) = 0;

    /** Bind the HDR offscreen render target WITHOUT clearing it, and optionally bind a depth buffer.
     *  Use for passes (e.g. SkyboxPass) that draw on top of existing HDR content.
     *  @p dsv — optional depth-stencil texture (must have DEPTH_STENCIL bind flag). */
    virtual void SetRenderTargetToHdrWithDepth(const RHI::Texture* dsv,
                                               RHI::CommandList    cmd) = 0;

    /** Bind the swap-chain back buffer and clear it.
     *  Records into @p cmd. Call before ImGui rendering. */
    virtual void SetRenderTargetToSwapChain(const float clearColor[4],
                                            RHI::CommandList cmd) = 0;

    /** Draw a fullscreen triangle that samples @p srvGpuHandle and writes to
     *  the currently-bound swap-chain RTV. Intended as the Game-mode (no
     *  ImGui) alternative to ImGui's fullscreen Image composite.
     *  Must be called after SetRenderTargetToSwapChain, on the same CL. */
    virtual void CompositeTextureToSwapChain(uint64_t         srvGpuHandle,
                                             RHI::CommandList cmd) = 0;

    // =========================================================================
    // Queries
    // =========================================================================

    virtual uint32_t GetWidth()  const = 0;
    virtual uint32_t GetHeight() const = 0;

    /** Returns the current render viewport size (HDR offscreen target dimensions).
     *  May be smaller than the swap chain when the editor viewport panel is active.
     *  Falls back to GetWidth/GetHeight if SetViewportSize has not been called yet. */
    virtual uint32_t GetRenderWidth()  const { return GetWidth();  }
    virtual uint32_t GetRenderHeight() const { return GetHeight(); }

    /** GPU handle of the HDR SRV as uint64_t (ImTextureID / D3D12_GPU_DESCRIPTOR_HANDLE::ptr). */
    virtual uint64_t GetHdrSceneSrvGpuHandle() const = 0;

    /** GPU handle of the HDR UAV. Returns 0 if the backend didn't allocate one
     *  (i.e. HDR isn't UAV-bindable). Used by SSR composite to write reflections
     *  directly into the HDR scene after lighting + skybox finish. */
    virtual uint64_t GetHdrSceneUavGpuHandle() const { return 0; }

    /** Copy the full HDR scene target into @p dst (must have identical format
     *  and dimensions). Caller owns barriers: HDR must be COPY_SOURCE and @p dst
     *  must be COPY_DEST before the call. Used by SSR composite to snapshot HDR
     *  so the compute pass can safely read it while writing to the same UAV. */
    virtual void CopyHdrSceneTo(const RHI::Texture& dst, RHI::CommandList cmd) {}

    /** GPU handle of any texture's default SRV as uint64_t.
     *  Returns 0 if the texture is invalid or has no SRV.
     *  Use as ImTextureID for ImGui::Image() thumbnail rendering. */
    virtual uint64_t GetTextureSRVGpuHandle(const RHI::Texture& texture) const { return 0; }

    /** GPU handle of a texture's UNORM alias SRV (raw sRGB bytes, no
     *  linearisation). Used by editor previews that render to a UNORM RTV.
     *  Falls back to the default SRV when no alias exists. */
    virtual uint64_t GetTexturePreviewSrvGpuHandle(const RHI::Texture& texture) const
    { return GetTextureSRVGpuHandle(texture); }

    // =========================================================================
    // Descriptor table management
    // =========================================================================

    /** Allocate @p count consecutive shader-visible SRV descriptors, one per depth
     *  texture, each viewed as DXGI_FORMAT_R32_FLOAT (D32 read as float).
     *  Copies the descriptors to the GPU-visible heap and returns the GPU handle
     *  of the first entry (use with BindDescriptorTableGpuHandle).
     *  Returns 0 on failure. The handle is valid until FreeDescriptorTable is called. */
    virtual uint64_t CreateDepthTextureSRVTable(const RHI::Texture* textures,
                                                uint32_t            count) = 0;

    /** Release a descriptor table previously allocated by CreateDepthTextureSRVTable. */
    virtual void FreeDescriptorTable(uint64_t gpuHandle) = 0;

    // =========================================================================
    // Resource creation
    // =========================================================================

    virtual bool CreateBuffer(
        const RHI::GPUBufferDesc& desc,
        RHI::GPUBuffer&           outBuffer,
        const void*               initialData = nullptr) = 0;

    /** Begin/end a buffer-upload batch scope. Within the scope, CreateBuffer
     *  calls that would otherwise `FlushUploadAndWait()` per buffer defer
     *  the fence wait until EndBufferUploadBatch(). Used to cut down the
     *  per-buffer GPU-fence roundtrip when loading thousands of meshes — the
     *  single biggest cost of bulk scene load.
     *
     *  Nesting is NOT supported (the default impl is a flat counter).
     *  Batches must be ended on the same thread that began them. */
    virtual void BeginBufferUploadBatch() {}
    virtual void EndBufferUploadBatch()   {}

    virtual bool CreateTexture(
        const RHI::TextureDesc&      desc,
        RHI::Texture&                outTexture,
        const RHI::SubresourceData*  initialData = nullptr) = 0;

    virtual bool CreateShader(
        RHI::ShaderStage stage,
        const void*      bytecode,
        size_t           bytecodeSize,
        RHI::Shader&     outShader) = 0;

    virtual bool CreateSampler(
        const RHI::SamplerDesc& desc,
        int&                    outDescriptorIndex) = 0;

    virtual bool CreatePipelineState(
        const RHI::PipelineStateDesc& desc,
        RHI::PipelineState&           outPSO) = 0;

    // =========================================================================
    // Command recording — state        (all require a valid CommandList)
    // =========================================================================

    virtual void BindPipelineState(const RHI::PipelineState& pso,
                                   RHI::CommandList cmd) = 0;

    virtual void SetPrimitiveTopology(RHI::PrimitiveTopology topology,
                                      RHI::CommandList cmd) = 0;

    virtual void SetViewport(const RHI::Viewport& viewport,
                             RHI::CommandList cmd) = 0;

    virtual void SetScissorRect(uint32_t left, uint32_t top,
                                uint32_t right, uint32_t bottom,
                                RHI::CommandList cmd) = 0;

    // =========================================================================
    // Command recording — resource binding  (slot → HLSL register b/t/s index)
    // =========================================================================

    virtual void BindConstantBuffer(const RHI::GPUBuffer& buffer,
                                    uint32_t slot,
                                    RHI::CommandList cmd) = 0;

    virtual void BindResource(const RHI::GPUResource& resource,
                              uint32_t slot,
                              RHI::CommandList cmd) = 0;

    virtual void BindSampler(int descriptorIndex,
                             uint32_t slot,
                             RHI::CommandList cmd) = 0;

    // =========================================================================
    // Command recording — PVF (Programmable Vertex Fetching) bindings
    // =========================================================================

    /** Push 3 root constants (meshDescIdx, instanceOffset, materialIndex) at root param 0. */
    virtual void SetRootConstants(uint32_t v0, uint32_t v1, uint32_t v2,
                                  RHI::CommandList cmd) = 0;

    /** Bind a GPU buffer as a root SRV at the given root parameter slot.
     *  Works for both DEFAULT and UPLOAD heap StructuredBuffers / ByteAddressBuffers.
     *  @p rootSlot — absolute root parameter index (not an API slot). */
    virtual void SetRootBufferSRV(const RHI::GPUBuffer& buf,
                                  uint32_t rootSlot,
                                  RHI::CommandList cmd) = 0;

    /** Bind a descriptor table using a raw GPU handle (e.g. for the bindless g_Buffers[] table).
     *  @p rootSlot — absolute root parameter index. */
    virtual void BindDescriptorTableGpuHandle(uint32_t rootSlot,
                                              uint64_t gpuHandle,
                                              RHI::CommandList cmd) = 0;

    // =========================================================================
    // Command recording — draws
    // =========================================================================

    virtual void DrawInstanced(uint32_t vertexCount,
                               uint32_t instanceCount,
                               uint32_t startVertex,
                               uint32_t startInstance,
                               RHI::CommandList cmd) = 0;

    // GPU-driven draw: execute indirect draw commands from a GPU buffer.
    // argBuffer contains IndirectDrawCommand entries.
    // countBuffer (optional, 0 = use maxCommands) contains a uint32 GPU-side draw count.
    virtual void ExecuteIndirectDraw(const RHI::GPUBuffer& argBuffer,
                                     uint64_t argBufferOffset,
                                     uint32_t maxCommands,
                                     const RHI::GPUBuffer* countBuffer,
                                     uint64_t countBufferOffset,
                                     RHI::CommandList cmd) = 0;

    // Copy 'size' bytes from src buffer to dst buffer.
    virtual void CopyBuffer(const RHI::GPUBuffer& src, const RHI::GPUBuffer& dst,
                            uint64_t size, RHI::CommandList cmd) = 0;

    /** Copy one (mip, arraySlice) subresource from src to dst.
     *  Both textures must share the same format and per-mip dimensions at the
     *  requested level. The caller owns barriers — src must be COPY_SOURCE and
     *  dst must be COPY_DEST before this call. Used by the reflection probe
     *  broadcast to fan a single TextureCube into one cube of a TextureCubeArray. */
    virtual void CopyTextureSubresource(const RHI::Texture& src,
                                        uint32_t srcMip, uint32_t srcArraySlice,
                                        const RHI::Texture& dst,
                                        uint32_t dstMip, uint32_t dstArraySlice,
                                        RHI::CommandList cmd) {}

    virtual void DrawIndexedInstanced(uint32_t indexCount,
                                      uint32_t instanceCount,
                                      uint32_t startIndex,
                                      int32_t  baseVertex,
                                      uint32_t startInstance,
                                      RHI::CommandList cmd) = 0;

    // =========================================================================
    // Command recording — compute dispatch
    // =========================================================================

    /** Bind a compute pipeline state and its compute root signature. */
    virtual void BindComputePipelineState(const RHI::PipelineState& pso,
                                          RHI::CommandList cmd) {}

    /** Dispatch compute threads. */
    virtual void DispatchCompute(uint32_t threadGroupX,
                                 uint32_t threadGroupY,
                                 uint32_t threadGroupZ,
                                 RHI::CommandList cmd) {}

    /** Dispatch mesh-shader thread groups (DX12 Ultimate, SM 6.5+).
     *  When an Amplification Shader is bound the AS is launched with these
     *  group counts; when only an MS is bound, the MS is launched directly.
     *  No-op on backends without mesh-shader support. */
    virtual void DispatchMesh(uint32_t threadGroupX,
                              uint32_t threadGroupY,
                              uint32_t threadGroupZ,
                              RHI::CommandList cmd) {}

    /** Bind a GPU buffer as a compute root inline CBV (root parameter @p slot). */
    virtual void SetComputeRootCBV(uint32_t rootSlot,
                                   const RHI::GPUBuffer& cb,
                                   uint32_t byteOffset,
                                   RHI::CommandList cmd) {}
    virtual void SetComputeRootCBV(uint32_t rootSlot,
                                   const RHI::GPUBuffer& cb,
                                   RHI::CommandList cmd)
    { SetComputeRootCBV(rootSlot, cb, 0, cmd); }

    /** Bind a descriptor table to the compute root signature. */
    virtual void SetComputeDescriptorTable(uint32_t rootSlot,
                                           uint64_t gpuHandle,
                                           RHI::CommandList cmd) {}

    /** Returns the GPU descriptor handle for a texture's UAV (space2).
     *  Returns 0 if the texture was not created with UNORDERED_ACCESS bind flag. */
    virtual uint64_t GetTextureUAVGpuHandle(const RHI::Texture& tex) const { return 0; }

    /** Returns the GPU descriptor handle for a buffer's UAV.
     *  Returns 0 if the buffer was not created with UNORDERED_ACCESS bind flag. */
    virtual uint64_t GetBufferUAVGpuHandle(const RHI::GPUBuffer& buf) const { return 0; }

    /** Returns the GPU descriptor handle for a buffer's SRV.
     *  Returns 0 if the buffer was not created with SHADER_RESOURCE bind flag. */
    virtual uint64_t GetBufferSRVGpuHandle(const RHI::GPUBuffer& buf) const { return 0; }

    /** GPU descriptor handle of the backend's bindless texture table (t0 space100
     *  under the current default root signature). Used by passes that bind
     *  g_AllTextures[] as a descriptor table. Returns 0 if the backend does
     *  not allocate a bindless table. */
    virtual uint64_t GetBindlessTextureTableGpuHandle() const { return 0; }

    /** CPU descriptor handle of a texture's SRV (for descriptor-table staging
     *  such as custom-material SRV rings). Returns 0 if the texture has no SRV
     *  or the backend doesn't expose CPU handles. */
    virtual uint64_t GetTextureSRVCpuHandle(const RHI::Texture& tex) const { return 0; }

    /** Copy @p count CBV/SRV/UAV descriptors from @p srcCpuHandle into
     *  @p dstCpuHandle. Used by frame descriptor-table builders. The caller is
     *  responsible for advancing the destination handle by descriptor-increment
     *  bytes between individual copies. */
    virtual void CopyCbvSrvUavDescriptors(uint64_t dstCpuHandle,
                                          uint64_t srcCpuHandle,
                                          uint32_t count) {}

    /** Backend descriptor increment (in bytes) for the CBV/SRV/UAV heap —
     *  needed by code that composes a descriptor table slot-by-slot. Returns 0
     *  if the backend doesn't use a CPU descriptor heap. */
    virtual uint32_t GetCbvSrvUavDescriptorIncrement() const { return 0; }

    /** Transition the HDR offscreen render target to @p newState.
     *  Emits the resource barrier into @p cmd and updates internal state tracking. */
    virtual void SetHdrTextureState(RHI::ResourceState newState,
                                    RHI::CommandList cmd) {}

    // =========================================================================
    // Command recording — barriers
    // =========================================================================

    virtual void PushBarrier(const RHI::GPUBarrier& barrier,
                             RHI::CommandList cmd) = 0;

    // =========================================================================
    // Command recording — render targets
    // =========================================================================

    /** Bind multiple render targets and an optional depth-stencil. */
    virtual void SetRenderTargets(uint32_t numRTs,
                                  const RHI::Texture* const* rtvs,
                                  const RHI::Texture*        dsv,
                                  RHI::CommandList           cmd) = 0;

    /** Bind multiple render targets PLUS the device-internal HdrSceneColor
     *  appended as the LAST RT (RT[numRTs]). Used by GBufferPass / TerrainPass
     *  to write per-material emissive directly to the scene color target
     *  (Unreal-style: emissive bypasses the BRDF, lighting adds on top via
     *  additive blend). HDR is auto-transitioned to RENDER_TARGET if needed. */
    virtual void SetRenderTargetsAndHdr(uint32_t numRTs,
                                        const RHI::Texture* const* rtvs,
                                        const RHI::Texture*        dsv,
                                        RHI::CommandList           cmd) = 0;

    virtual void ClearRenderTarget(const RHI::Texture& texture,
                                   const float         color[4],
                                   RHI::CommandList    cmd) = 0;

    /** Clear the device-internal HdrSceneColor RT to @p color. Must be called
     *  while HDR is in RENDER_TARGET state (use SetRenderTargetsAndHdr first). */
    virtual void ClearHdrRenderTarget(const float      color[4],
                                      RHI::CommandList cmd) = 0;

    virtual void ClearDepthStencil(const RHI::Texture& texture,
                                   float               depth,
                                   uint8_t             stencil,
                                   RHI::CommandList    cmd) = 0;

    // =========================================================================
    // Command recording — array-slice depth targets (CSM cascades, etc.)
    // =========================================================================

    /** Bind one array slice of a DEPTH_STENCIL Texture2DArray as the only DSV.
     *  RT list is cleared (depth-only pass). Texture must already be in
     *  DEPTHSTENCIL state and have been created with DEPTH_STENCIL bind flag,
     *  array_size > 1. */
    virtual void SetDepthStencilSlice(const RHI::Texture& depthTex,
                                      uint32_t            arraySlice,
                                      RHI::CommandList    cmd) {}

    /** Clear one array slice of a DEPTH_STENCIL Texture2DArray.
     *  Must be called while the texture is in DEPTHSTENCIL state. */
    virtual void ClearDepthStencilSlice(const RHI::Texture& depthTex,
                                        uint32_t            arraySlice,
                                        float               depth,
                                        uint8_t             stencil,
                                        RHI::CommandList    cmd) {}

    // =========================================================================
    // Command recording — root constants / stencil ref
    // =========================================================================

    /** Set the stencil reference value for upcoming draws. */
    virtual void SetStencilRef(uint32_t ref, RHI::CommandList cmd) {}

    /** Write a single 32-bit value into the graphics root constants block at
     *  @p rootSlot, at word offset @p offsetInWords. Used for pushing per-draw
     *  scalar values (e.g. OutlinePass outlinePixels). */
    virtual void SetGraphicsRootConstant(uint32_t rootSlot,
                                         uint32_t value,
                                         uint32_t offsetInWords,
                                         RHI::CommandList cmd) {}

    // =========================================================================
    // Per-subresource descriptor queries
    // =========================================================================

    /** GPU descriptor handle of the UAV for a specific mip level of a
     *  multi-mip texture. For Texture2DArray / TextureCube the UAV covers
     *  all array slices at that mip. Returns 0 if the mip index is out of
     *  range or the texture was not created with UAV bind flag. */
    virtual uint64_t GetTextureMipUAVGpuHandle(const RHI::Texture& tex,
                                               uint32_t mip) const { return 0; }

    /** CPU descriptor handle of an RTV that targets exactly one (cubeIdx,
     *  face, mip) subresource of a TextureCubeArray. Lazily allocated on first
     *  request and cached on the texture entry. Used by the reflection probe
     *  capture pass to render the scene into one cube face at a time.
     *  Returns 0 if the subresource is out of range or RTV bind flag is missing. */
    virtual uint64_t GetTextureCubeFaceRTVCpuHandle(const RHI::Texture& tex,
                                                    uint32_t cubeIdx,
                                                    uint32_t face,
                                                    uint32_t mip) { return 0; }

    /** GPU descriptor handle of a Texture2DArray UAV that views the 6 slices
     *  [cubeIdx*6 .. cubeIdx*6+5] at a single mip level — i.e., one cube of a
     *  TextureCubeArray exposed as a 6-layer array UAV. Used by the prefilter
     *  compute shader to write GGX-filtered output into one probe's mip. */
    virtual uint64_t GetTextureCubeMipUAVGpuHandle(const RHI::Texture& tex,
                                                   uint32_t cubeIdx,
                                                   uint32_t mip) { return 0; }

    // =========================================================================
    // GPU profiler hooks
    //
    // Thin wrappers so passes can record timestamp regions without casting to
    // the concrete device. Default impls are no-ops (headless / non-profiled
    // back-ends); GraphicsDX12 forwards to its internal GPUProfiler.
    // =========================================================================

    /** Returns an opaque region index, or ~0u when profiling is disabled. */
    virtual uint32_t BeginGPUTimestamp(RHI::CommandList /*cmd*/,
                                       const char* /*name*/) { return ~0u; }
    virtual void     EndGPUTimestamp  (RHI::CommandList /*cmd*/,
                                       uint32_t /*regionIndex*/) {}
    virtual bool     IsGPUProfilerEnabled() const { return false; }

    // =========================================================================
    // Command recording — descriptor heap management
    // =========================================================================

    /** Bind the shader-visible CBV/SRV/UAV and sampler heaps.
     *  Must be called before any BindResource / BindSampler call. */
    virtual void BindDescriptorHeaps(RHI::CommandList cmd) = 0;

    // =========================================================================
    // Buffer mapping (for UPLOAD-heap constant/staging buffers)
    // =========================================================================

    /** Map an UPLOAD buffer and return a CPU pointer.
     *  The pointer remains valid until UnmapBuffer is called. */
    virtual void* MapBuffer(const RHI::GPUBuffer& buffer) = 0;

    /** Unmap an UPLOAD buffer that was previously mapped. */
    virtual void  UnmapBuffer(const RHI::GPUBuffer& buffer) = 0;

    // =========================================================================
    // Resource destruction
    // =========================================================================

    /** Copy the 1×1 pixel at (srcX, srcY) from a 2D texture (mip 0) to the start
     *  of a READBACK buffer.  Texture must be in COPY_SRC state before calling. */
    virtual void CopyTexturePixelToBuffer(const RHI::Texture& src,
                                          uint32_t srcX, uint32_t srcY,
                                          RHI::GPUBuffer& dst,
                                          RHI::CommandList cmd) = 0;

    /** Free all GPU resources associated with a buffer and invalidate the handle. */
    virtual void DestroyBuffer(RHI::GPUBuffer& buffer) = 0;

    /** Free all GPU resources associated with a texture and invalidate the handle. */
    virtual void DestroyTexture(RHI::Texture& texture) = 0;

    // =========================================================================
    // PSO library (ID3D12PipelineLibrary) — disk-based ISA cache.
    // Call InitPSOLibrary at startup and SavePSOLibrary at clean shutdown.
    // Both are no-ops if the backend does not support PipelineLibrary
    // (e.g. feature level < 12_1).
    // =========================================================================

    /** Load PSO library cache from disk.
     *  Gracefully handles missing file, corrupt data, or driver update
     *  (will simply start with an empty library and recompile ISA on first use). */
    virtual bool InitPSOLibrary(const char* cacheFilePath) = 0;

    /** Serialize all compiled PSOs to disk for fast warm-up on next launch. */
    virtual void SavePSOLibrary(const char* cacheFilePath) = 0;

    /** Capture an R8G8B8A8 texture to a PNG file via READBACK heap.
     *  Used by ShaderLab's "Capture Frame" button — synchronous (FlushAndWait
     *  inside) so the resulting file is always consistent with the moment of
     *  the call. Caller passes the texture's current resource state so the
     *  backend can transition through COPY_SOURCE and back without disturbing
     *  the renderer's own state tracker. Returns false on missing handle,
     *  unsupported format, or I/O failure. */
    virtual bool CaptureTextureToPNG(const RHI::Texture& tex,
                                     RHI::ResourceState  currentState,
                                     const char*         path) = 0;
};
