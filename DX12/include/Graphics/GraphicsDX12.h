#pragma once

// GraphicsDX12: Direct3D 12 implementation of IGraphicsDevice.
//
// Root signature layout (PVF — Programmable Vertex Fetching):
//   [0]    ROOT_CONSTANTS 3×uint32 b0 space0 → meshDescIdx · instanceOffset · materialIndex
//   [1..7] ROOT_CBV       b1-b7 space0       → BindConstantBuffer(slot 0-6)
//   [8]    ROOT_SRV       t0 space0           → SetRootBufferSRV (InstanceBuffer)
//   [9]    ROOT_SRV       t1 space0           → SetRootBufferSRV (MeshDescriptors)
//   [10..13] DESC_TABLE   1 SRV t2-t5 space0 → BindResource(slot 0-3)
//   [14]   DESC_TABLE     16384 SRV t0 space1 → bindless g_Buffers[] (kMaxBindlessBuffers)
//   [15..18] DESC_TABLE   1 sampler s0-s3     → BindSampler(slot 0-3)
//
// Compute root signature layout (space2):
//   [0]  ROOT_CBV       b0 space2  → SetComputeRootCBV(0, ...)
//   [1]  DESC_TABLE     1 SRV t0 space2 → SetComputeDescriptorTable(1, srvHandle)
//   [2]  DESC_TABLE     1 SRV t1 space2
//   [3]  DESC_TABLE     1 SRV t2 space2
//   [4]  DESC_TABLE     1 UAV u0 space2 → SetComputeDescriptorTable(4, uavHandle)
//   [5]  DESC_TABLE     1 UAV u1 space2
//   [6]  DESC_TABLE     1 SRV t5 space2 (morph weights)
//   [7]  DESC_TABLE     1 SRV t3 space2 (scene depth / prefilter source)
//   [8]  DESC_TABLE     1 SRV t4 space2 (scene occupancy)
//   [9]  DESC_TABLE     1 SRV t0 space0 (instance buffer)
//   [10] DESC_TABLE     1 SRV t1 space0 (mesh descriptors)
//   [11] DESC_TABLE     N SRV t0 space1 (bindless buffers)
//   [12] DESC_TABLE     1 SRV t6 space2 (spot shadow atlas)
//   [13] DESC_TABLE     1 SRV t7 space2 (spot shadow VP buffer)
//   [14] DESC_TABLE     1 UAV u2 space2 ─┐  — XeGTAO prefilter mip 2
//   [15] DESC_TABLE     1 UAV u3 space2  ├─ XeGTAO prefilter mip 3
//   [16] DESC_TABLE     1 UAV u4 space2 ─┘  — XeGTAO prefilter mip 4
//   static samplers: s0 space2 (linear clamp), s1 space2 (comparison PCF)
//
// Multi-threaded recording:
//   BeginFrame() returns CommandList{0} (primary).
//   BeginCommandList(queue) allocates from m_commandListPool[1..kMaxCommandLists-1].
//   Each CommandList_DX12 owns its own command allocators (per frame × per queue)
//   and command lists, created lazily on first use.
//   EndFrame() submits all active pool entries in allocation order.
//
// DX12-specific accessors (GetDevice, GetNativeCommandList, etc.) are non-virtual.
// A RenderPass that needs them should: static_cast<GraphicsDX12&>(gfx).

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <stdexcept>
#include <atomic>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <wrl.h>
#include "d3d12.h"
#include "dxgi1_6.h"
#include "d3dcompiler.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/DescriptorHeapAllocator.h"
#include "Graphics/GPUProfiler.h"
#include "System/Log.h"
#include "System/allocator.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#ifdef _DEBUG
#pragma comment(lib, "DirectXTex_Debug.lib")
#else
#pragma comment(lib, "DirectXTex_Release.lib")
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
namespace RHI::DX12 { class VideoDecoderDX12; }

// ---------------------------------------------------------------------------
// ThrowIfFailed helper (used by GraphicsDX12 and RenderPass implementations)
// ---------------------------------------------------------------------------
void ThrowIfFailedImpl(HRESULT hr, const char* expr, const char* file, int line);

#define ThrowIfFailed(expr) \
    ThrowIfFailedImpl((expr), #expr, __FILE__, __LINE__)


// ---------------------------------------------------------------------------
// CommandList_DX12
//
// Internal per-recording-session state.  One instance lives in
// GraphicsDX12::m_commandListPool; callers hold the opaque RHI::CommandList
// handle (just an index into that pool).
//
// Semaphore / dependency ordering:
//   wait_for          — pool IDs of command lists that must execute before this one.
//                       Populated via IGraphicsDevice::AddCommandListDependency().
//   signal_fence_value — written by EndFrame after this CL is submitted on its queue.
//                       Dependents on other queues Wait() on this value before
//                       submitting themselves.
// ---------------------------------------------------------------------------
struct CommandList_DX12
{
    using graphics_command_list_version = ID3D12GraphicsCommandList6;

    static constexpr uint32_t BUFFERCOUNT = 3;   // == GraphicsDX12::FrameCount
    static constexpr uint32_t QUEUE_COUNT = 3;   // == RHI::QUEUE_TYPE::COUNT

    // One allocator per [frame-buffer-index][queue-type] — lazily created.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators[BUFFERCOUNT][QUEUE_COUNT];
    // One command list per queue type — lazily created and reused across frames.
    Microsoft::WRL::ComPtr<ID3D12CommandList>      commandLists[QUEUE_COUNT];

    uint32_t        buffer_index = 0;
    RHI::QUEUE_TYPE queue        = RHI::QUEUE_TYPE::GRAPHICS;
    uint32_t        id           = ~0u;

    // ---- Semaphore ordering ------------------------------------------------
    // Pool IDs of command lists this one depends on (must run after them).
    // Filled by AddCommandListDependency(); consumed and cleared by EndFrame().
    std::vector<uint32_t> wait_for;
    // Fence value signaled on this CL's queue after it is submitted.
    // Cross-queue dependents Wait() on this value before their own submission.
    // Written by EndFrame(); not valid until after EndFrame().
    uint64_t signal_fence_value = 0;

    // ---- Pipeline / draw-state tracking ------------------------------------
    D3D_PRIMITIVE_TOPOLOGY     prev_pt                 = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    const RHI::PipelineState*  active_pso              = nullptr;
    const ID3D12RootSignature* active_rootsig_graphics = nullptr;
    const ID3D12RootSignature* active_rootsig_compute  = nullptr;
    uint32_t                   prev_stencilref         = 0;
    bool                       dirty_pso               = false;

    // ---- Pending resource barriers (batched before draw) -------------------
    std::vector<D3D12_RESOURCE_BARRIER> frame_barriers;

    // ---- Render-pass begin / end barriers ----------------------------------
    std::vector<D3D12_RESOURCE_BARRIER> renderpass_barriers_begin;
    std::vector<D3D12_RESOURCE_BARRIER> renderpass_barriers_end;

    // ---- Resources to discard at end of render pass -----------------------
    struct Discard
    {
        ID3D12Resource*      resource = nullptr;
        D3D12_DISCARD_REGION region   = {};
    };
    std::vector<Discard> discards;

    // ---- MSAA resolve targets (up to D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
    static constexpr int kMaxRTs = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
    ID3D12Resource* resolve_src[kMaxRTs]     = {};
    ID3D12Resource* resolve_dst[kMaxRTs]     = {};
    DXGI_FORMAT     resolve_formats[kMaxRTs] = {};
    std::vector<D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS>
        resolve_subresources[kMaxRTs];

    // ---- Helpers -----------------------------------------------------------
    bool IsValid() const noexcept { return id != ~0u; }

    graphics_command_list_version* GetCommandList() const noexcept
    {
        return static_cast<graphics_command_list_version*>(
            commandLists[static_cast<UINT>(queue)].Get());
    }
};


class GraphicsDX12 : public IGraphicsDevice
{
public:
    static constexpr UINT FrameCount  = 3;
    static constexpr UINT MaxSamplers = 64;

    GraphicsDX12(HWND hWnd, UINT width, UINT height);
    ~GraphicsDX12() override;

    // =========================================================================
    // IGraphicsDevice — frame lifecycle
    // =========================================================================
    void             WaitForNextFrameSlot() override;
    RHI::CommandList BeginFrame() override;
    void             EndFrame()   override;
    void             FlushAndWait() override;
    void             WaitIdleAndReleaseDeferred() override { WaitForPreviousFrame(); }
    bool             CaptureTextureToPNG(const RHI::Texture& tex,
                                         RHI::ResourceState  currentState,
                                         const char*         path) override;

    // Lazy: first call constructs the DX12 video backend (QIs ID3D12VideoDevice,
    // spins up the video queue/fence). Returns nullptr when the driver does
    // not expose ID3D12VideoDevice or queue creation fails — callers should
    // gracefully fall back to no-video.
    RHI::IVideoDecoderBackend* GetVideoBackend() override;

    // =========================================================================
    // IGraphicsDevice — command list management
    // =========================================================================
    RHI::CommandList BeginCommandList(
        RHI::QUEUE_TYPE queue = RHI::QUEUE_TYPE::GRAPHICS) override;

    // Declare that @p waiter must execute after @p dependency on the GPU.
    // Same-queue: enforced by submission order.
    // Cross-queue: a GPU fence Wait/Signal is inserted in EndFrame.
    void AddCommandListDependency(RHI::CommandList waiter,
                                  RHI::CommandList dependency) override;

    // =========================================================================
    // IGraphicsDevice — window/swap chain
    // =========================================================================
    void Resize(uint32_t width, uint32_t height)  override;
    void SetFullscreen(bool fullscreen)            override;
    void SetViewportSize(uint32_t width, uint32_t height) override;
    void SetRenderTargetToHdr(const float clearColor[4],
                              RHI::CommandList cmd) override;
    void SetRenderTargetToHdrWithDepth(const RHI::Texture* dsv,
                                       RHI::CommandList    cmd) override;
    void SetRenderTargetToSwapChain(const float clearColor[4],
                                    RHI::CommandList cmd) override;
    void CompositeTextureToSwapChain(uint64_t         srvGpuHandle,
                                     RHI::CommandList cmd) override;

    uint32_t GetWidth()  const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetRenderWidth()  const override { return m_hdrWidth  ? m_hdrWidth  : m_width;  }
    uint32_t GetRenderHeight() const override { return m_hdrHeight ? m_hdrHeight : m_height; }
    uint32_t GetFrameIndex()   const override { return m_frameIndex; }
    // Note: pre-existing static constexpr GetFrameCount() lives further down
    // in this class; we don't shadow it with a virtual to keep the existing
    // static call sites working. FrameCB.h hard-codes kFrameCount=3 to match.
    uint64_t GetHdrSceneSrvGpuHandle() const override;
    uint64_t GetHdrSceneUavGpuHandle() const override;
    void     CopyHdrSceneTo(const RHI::Texture& dst, RHI::CommandList cmd) override;
    uint64_t GetTextureSRVGpuHandle(const RHI::Texture& texture) const override;
    uint64_t GetTextureSRVCpuHandle(const RHI::Texture& texture) const override;
    // DX12-native counterpart — used internally to stage descriptors into a
    // ring (e.g. custom-material texture table) via the raw D3D12 handle.
    // Returns a zero handle when the texture is unloaded.
    D3D12_CPU_DESCRIPTOR_HANDLE GetTextureSRVCpuHandleNative(const RHI::Texture& texture) const;
    uint64_t GetBindlessTextureTableGpuHandle() const override { return m_bindlessTexTable.GetGpuHandle().ptr; }
    void     CopyCbvSrvUavDescriptors(uint64_t dstCpuHandle,
                                      uint64_t srcCpuHandle,
                                      uint32_t count) override;
    uint32_t GetCbvSrvUavDescriptorIncrement() const override;
    // Returns a UNORM alias SRV for SRGB textures (raw sRGB data, no linearization).
    // For editor texture previews rendered to a UNORM RTV.
    uint64_t GetTexturePreviewSrvGpuHandle(const RHI::Texture& texture) const override;
    // Stencil-plane SRV for D24_UNORM_S8_UINT depth textures. Created lazily in
    // CreateTexture alongside the depth-plane SRV. Returns 0 if the texture is
    // not stencil-bearing. Sampled as Texture2D<uint2>; read .y for stencil.
    uint64_t GetTextureStencilSRVGpuHandle(const RHI::Texture& texture) const override;
    uint64_t GetTextureUVPlaneSRVGpuHandle(const RHI::Texture& texture) const override;
    uint64_t CreateDepthTextureSRVTable(const RHI::Texture* textures,
                                        uint32_t            count) override;
    void     FreeDescriptorTable(uint64_t gpuHandle) override;

    // =========================================================================
    // IGraphicsDevice — resource creation
    // =========================================================================
    bool CreateBuffer(
        const RHI::GPUBufferDesc& desc,
        RHI::GPUBuffer&           outBuffer,
        const void*               initialData = nullptr) override;

    void BeginBufferUploadBatch() override;
    void EndBufferUploadBatch()   override;

    bool CreateTexture(
        const RHI::TextureDesc&     desc,
        RHI::Texture&               outTexture,
        const RHI::SubresourceData* initialData = nullptr) override;

    // DX12-only: create a TRANSIENT 2D texture as a CreatePlacedResource at
    // (heap, heapOffset) instead of its own committed heap, so lifetime-disjoint
    // textures can alias the same heap bytes (transient render-target aliasing).
    // Narrow on purpose — single-mip, single-slice, SHADER_RESOURCE (+optional
    // UNORDERED_ACCESS) only; NO initialData / RT / DS / cube / depth / NV12.
    // The resulting RHI::Texture is otherwise indistinguishable from a committed
    // one (same SRV/UAV descriptors, same bindless slot). Caller owns the heap
    // and must keep it alive until this texture's deferred release has drained.
    bool CreateTexturePlaced(
        const RHI::TextureDesc&     desc,
        ID3D12Heap*                 heap,
        UINT64                      heapOffset,
        RHI::Texture&               outTexture);

    bool UpdateTexture(
        RHI::Texture&               texture,
        const RHI::SubresourceData* planes,
        uint32_t                    subresourceCount) override;

    /** GPU-to-GPU copy of an external ID3D12Resource (e.g. FFmpeg's D3D12VA
     *  hwaccel NV12 output) into a managed RHI::Texture. The graphics queue
     *  inserts Wait(waitFence, waitValue) BEFORE the copy CL submission so
     *  the producer's work is retired before we read. Then synchronously
     *  waits for the copy to finish (FlushUploadAndWait-style) so the
     *  destination is safe to sample from any subsequent CL.
     *
     *  Both resources must share the same format + dimensions (CopyResource
     *  internally maps all subresources). Pass nullptr / 0 for waitFence to
     *  skip the GPU wait (use only when the caller guarantees the producer
     *  has already retired). Returns false when texture handle / src is
     *  invalid. DX12-only; cast IGraphicsDevice& to GraphicsDX12& to call. */
    bool CopyD3D12ResourceToTexture(RHI::Texture&    dst,
                                     ID3D12Resource* src,
                                     ID3D12Fence*    waitFence,
                                     UINT64          waitFenceValue);

    bool CreateShader(
        RHI::ShaderStage stage,
        const void*      bytecode,
        size_t           bytecodeSize,
        RHI::Shader&     outShader) override;

    bool CreateSampler(
        const RHI::SamplerDesc& desc,
        int&                    outDescriptorIndex) override;

    bool CreatePipelineState(
        const RHI::PipelineStateDesc& desc,
        RHI::PipelineState&           outPSO) override;

    // =========================================================================
    // IGraphicsDevice — command recording: state
    // =========================================================================
    void BindPipelineState(const RHI::PipelineState& pso,
                           RHI::CommandList cmd) override;
    void SetPrimitiveTopology(RHI::PrimitiveTopology topology,
                              RHI::CommandList cmd) override;
    void SetViewport(const RHI::Viewport& viewport,
                     RHI::CommandList cmd) override;
    void SetScissorRect(uint32_t left, uint32_t top,
                        uint32_t right, uint32_t bottom,
                        RHI::CommandList cmd) override;

    // =========================================================================
    // IGraphicsDevice — command recording: resource binding
    // =========================================================================
    void BindConstantBuffer(const RHI::GPUBuffer& buffer,
                            uint32_t slot,
                            RHI::CommandList cmd) override;

    // Slot-offset variant for ring-buffer style CB uploads (e.g. probe capture
    // pass uploading 6 face CBs into one 1.5KB ring).
    void BindConstantBufferAtOffset(uint32_t slot,
                                    const RHI::GPUBuffer& buffer,
                                    uint64_t byteOffset,
                                    RHI::CommandList cmd);

    // Phase E custom-material CBV bind (root slot 35, register b8 space0).
    // Called per-draw with a GPU VA produced by MaterialCBVRing; GBufferPass
    // is the primary caller. Standard shaders that don't declare b8 are
    // unaffected when this isn't called.
    void BindCustomMaterialCBV(uint64_t gpuVA, RHI::CommandList cmd);

    // Phase F custom-material texture table bind (root slot 36, t0-t7 space3).
    // gpuHandle points at the base of a contiguous descriptor range allocated
    // via MaterialSRVRing::Allocate + populated with the shader's reflected
    // textures. Unset slots within the range default to white.
    void BindCustomMaterialTextureTable(uint64_t gpuHandle, RHI::CommandList cmd);

    // Number of slots the custom texture table exposes. Must match the root
    // sig range (see kCustomMatTexCount in GraphicsDX12.cpp) AND the C++
    // Resource::kMaxCustomTextures so reflection-driven binding never
    // overruns the table. Bumped 4→8 alongside Resource::kMaxCustomTextures
    // in M3.1 of the reflection-material refactor.
    static constexpr uint32_t kCustomMatTextureSlots = 8;
    void BindResource(const RHI::GPUResource& resource,
                      uint32_t slot,
                      RHI::CommandList cmd) override;
    void BindSampler(int descriptorIndex,
                     uint32_t slot,
                     RHI::CommandList cmd) override;

    // =========================================================================
    // IGraphicsDevice — PVF bindings
    // =========================================================================
    void SetRootConstants(uint32_t v0, uint32_t v1, uint32_t v2,
                          RHI::CommandList cmd) override;
    void SetRootBufferSRV(const RHI::GPUBuffer& buf,
                          uint32_t rootSlot,
                          RHI::CommandList cmd) override;
    void BindDescriptorTableGpuHandle(uint32_t rootSlot, uint64_t gpuHandle,
                                      RHI::CommandList cmd) override;

    // =========================================================================
    // IGraphicsDevice — draws + barriers  (no VB/IB — PVF eliminates IA)
    // =========================================================================
    void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                       uint32_t startVertex,  uint32_t startInstance,
                       RHI::CommandList cmd) override;
    void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                              uint32_t startIndex,  int32_t  baseVertex,
                              uint32_t startInstance,
                              RHI::CommandList cmd) override;
    void ExecuteIndirectDraw(const RHI::GPUBuffer& argBuffer,
                              uint64_t argBufferOffset,
                              uint32_t maxCommands,
                              const RHI::GPUBuffer* countBuffer,
                              uint64_t countBufferOffset,
                              RHI::CommandList cmd) override;
    void CopyBuffer(const RHI::GPUBuffer& src, const RHI::GPUBuffer& dst,
                    uint64_t size, RHI::CommandList cmd) override;
    void CopyTextureSubresource(const RHI::Texture& src,
                                uint32_t srcMip, uint32_t srcArraySlice,
                                const RHI::Texture& dst,
                                uint32_t dstMip, uint32_t dstArraySlice,
                                RHI::CommandList cmd) override;

    // =========================================================================
    // IGraphicsDevice — compute dispatch
    // =========================================================================
    void BindComputePipelineState(const RHI::PipelineState& pso,
                                  RHI::CommandList cmd) override;
    void DispatchCompute(uint32_t x, uint32_t y, uint32_t z,
                         RHI::CommandList cmd) override;
    void DispatchMesh(uint32_t x, uint32_t y, uint32_t z,
                      RHI::CommandList cmd) override;
    void SetComputeRootCBV(uint32_t rootSlot,
                           const RHI::GPUBuffer& cb,
                           uint32_t byteOffset,
                           RHI::CommandList cmd) override;
    using IGraphicsDevice::SetComputeRootCBV;
    void SetComputeDescriptorTable(uint32_t rootSlot,
                                   uint64_t gpuHandle,
                                   RHI::CommandList cmd) override;
    uint64_t GetTextureUAVGpuHandle(const RHI::Texture& tex) const override;
    uint64_t GetBufferUAVGpuHandle(const RHI::GPUBuffer& buf) const override;
    uint64_t GetBufferSRVGpuHandle(const RHI::GPUBuffer& buf) const override;
    void SetHdrTextureState(RHI::ResourceState newState,
                            RHI::CommandList cmd) override;

    void PushBarrier(const RHI::GPUBarrier& barrier,
                     RHI::CommandList cmd) override;

    void SetRenderTargets(uint32_t numRTs,
                          const RHI::Texture* const* rtvs,
                          const RHI::Texture*        dsv,
                          RHI::CommandList           cmd) override;

    void SetRenderTargetsAndHdr(uint32_t numRTs,
                                const RHI::Texture* const* rtvs,
                                const RHI::Texture*        dsv,
                                RHI::CommandList           cmd) override;

    void ClearRenderTarget(const RHI::Texture& texture,
                           const float         color[4],
                           RHI::CommandList    cmd) override;

    void ClearHdrRenderTarget(const float      color[4],
                              RHI::CommandList cmd) override;

    void ClearDepthStencil(const RHI::Texture& texture,
                           float               depth,
                           uint8_t             stencil,
                           RHI::CommandList    cmd) override;

    void SetDepthStencilSlice(const RHI::Texture& depthTex,
                              uint32_t            arraySlice,
                              RHI::CommandList    cmd) override;

    void ClearDepthStencilSlice(const RHI::Texture& depthTex,
                                uint32_t            arraySlice,
                                float               depth,
                                uint8_t             stencil,
                                RHI::CommandList    cmd) override;

    void SetStencilRef(uint32_t ref, RHI::CommandList cmd) override;

    void SetGraphicsRootConstant(uint32_t rootSlot,
                                 uint32_t value,
                                 uint32_t offsetInWords,
                                 RHI::CommandList cmd) override;

    uint64_t GetTextureMipUAVGpuHandle(const RHI::Texture& tex,
                                       uint32_t mip) const override;
    uint64_t GetTextureCubeFaceRTVCpuHandle(const RHI::Texture& tex,
                                            uint32_t cubeIdx, uint32_t face,
                                            uint32_t mip) override;
    uint64_t GetTextureCubeMipUAVGpuHandle(const RHI::Texture& tex,
                                           uint32_t cubeIdx, uint32_t mip) override;

    uint32_t BeginGPUTimestamp(RHI::CommandList cmd, const char* name) override;
    void     EndGPUTimestamp  (RHI::CommandList cmd, uint32_t regionIndex) override;
    bool     IsGPUProfilerEnabled() const override { return m_gpuProfiler.enabled; }

    void BindDescriptorHeaps(RHI::CommandList cmd) override;

    void* MapBuffer(const RHI::GPUBuffer& buffer) override;
    void  UnmapBuffer(const RHI::GPUBuffer& buffer) override;

    void CopyTexturePixelToBuffer(const RHI::Texture& src,
                                  uint32_t srcX, uint32_t srcY,
                                  RHI::GPUBuffer& dst,
                                  RHI::CommandList cmd) override;

    void DestroyBuffer(RHI::GPUBuffer& buffer) override;
    void DestroyTexture(RHI::Texture& texture) override;

    // Defer-release a raw ID3D12Resource (not pool-managed — e.g. DXR BLAS/TLAS
    // result buffers owned by RT::BLAS / RT::TLAS). Queues the ComPtr into the
    // current backbuffer's deferred-release slot so it outlives any command
    // list recorded this frame; BeginFrame drains it after the GPU is idle.
    // Use this instead of letting the owning ComPtr destruct inline whenever a
    // resource may still be referenced by an in-flight / not-yet-submitted CL.
    void DeferReleaseResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource);

    // Defer-release tied to a specific queue's fence value. Use when the
    // 3-slot per-backbuffer release window isn't enough — e.g. async-compute
    // BLAS work that may still be reading the resource long after the frame
    // it was queued in (without this, MeshLibrary needs a two-stage lag and
    // engine shutdown needs a full WaitForPreviousFrame stall). Resource is
    // released the first BeginFrame after `m_queueFences[queueIndex]`'s
    // completed value reaches `fenceValue`.
    //
    // queueIndex: 0 = graphics, 1 = compute, 2 = copy (matches
    // RHI::QUEUE_TYPE enum). Pass the value returned by
    // GetLastSignaledFenceValue() right AFTER the work using the resource
    // was submitted — that's the fence value the GPU must reach before
    // it's safe to free.
    void DeferReleaseResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource,
                              uint32_t queueIndex,
                              uint64_t fenceValue);

    // Last value signaled on a queue's fence. Caller can stash this right
    // after submitting work that references a resource, then pass it to
    // DeferReleaseResource(resource, queue, fenceValue) to time the
    // release precisely. queueIndex: 0/1/2 = graphics/compute/copy.
    uint64_t GetLastSignaledFenceValue(uint32_t queueIndex) const;

    // -------------------------------------------------------------------------
    // Cubemap-array slice <-> .itex file I/O (reflection-probe bake persistence).
    //
    // Uses the engine's native .itex container — [AssetHeader][TextureMetadata]
    // [DDS bytes] — exactly like TextureImporter / TextureLoader, so probe
    // cubemaps are first-class engine assets (no bespoke on-disk format).
    //
    // Both operate on ONE cube (6 consecutive array slices, all mips) inside a
    // TextureCubeArray — @p cubeIndex selects which probe; the touched array
    // slices are [cubeIndex*6 .. cubeIndex*6+5]. @p currentState is the state
    // the WHOLE array resource is in on entry (the reflection-probe array is
    // uniformly SHADER_RESOURCE outside an in-flight bake CL). Both are
    // synchronous: FlushAndWait + a dedicated one-shot CL + fence, mirroring
    // CaptureTextureToPNG — call them at frame boundaries / tools events, not
    // inside an open pass.
    // -------------------------------------------------------------------------
    bool SaveTextureCubeToITEX(const RHI::Texture& cubeArrayTex,
                               uint32_t            cubeIndex,
                               RHI::ResourceState  currentState,
                               const char*         path);
    bool LoadITEXIntoTextureCube(RHI::Texture&      cubeArrayTex,
                                 uint32_t           cubeIndex,
                                 RHI::ResourceState currentState,
                                 const char*        path);

    bool InitPSOLibrary(const char* cacheFilePath) override;
    void SavePSOLibrary(const char* cacheFilePath) override;

    // =========================================================================
    // DX12-specific accessors (for RenderPass implementations; cast IGraphicsDevice&)
    // =========================================================================
    ID3D12GraphicsCommandList* GetNativeCommandList(RHI::CommandList cmd) const;
    ID3D12Device*              GetDevice() const;
    // Exposed so the editor's ImGui bridge can bind backend-owned handles
    // without touching the engine's internal state directly.
    ID3D12CommandQueue*        GetGraphicsQueue()   const { return m_queues[0].Get(); }
    HWND                       GetHwnd()            const { return m_hWnd; }
    DXGI_FORMAT                GetSwapChainFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    static constexpr uint32_t  GetFrameCount()            { return FrameCount; }
    DescriptorHeapAllocator&   GetRtvAllocator();
    DescriptorHeapAllocator&   GetCbvSrvUavAllocator();
    DescriptorHeapAllocator&   GetDsvAllocator();
    // True if the device reports D3D12_MESH_SHADER_TIER_1 or higher. Queried
    // once during init from D3D12_FEATURE_DATA_D3D12_OPTIONS7. Terrain (and
    // any future MS pass) must skip Init when this is false.
    bool                       SupportsMeshShader() const { return m_meshShaderSupported; }
    // DXR raytracing capability — queried via D3D12_FEATURE_DATA_D3D12_OPTIONS5.
    // RaytracingTier_1_0 supports DispatchRays/AS/SBT; Tier_1_1 adds inline raytracing
    // and additional ray flags. DDGI uses Tier_1_0 for the probe trace pass.
    bool                       SupportsDXR() const { return m_dxrSupported; }
    // QI'd from m_device on demand. Returns nullptr when SupportsDXR()==false.
    // Used by Raytracing.{h,cpp} for AS build + state-object creation.
    struct ID3D12Device5*      GetDevice5() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetTextureDsvCpuHandle(const RHI::Texture& tex) const;
    /** Returns the underlying D3D12 resource for a GPUBuffer (needed by MeshDescriptorHeap). */
    ID3D12Resource*            GetBufferResource(const RHI::GPUBuffer& buffer) const;
    /** Returns the underlying D3D12 resource for a Texture (needed by ShadowPass SRV table). */
    ID3D12Resource*            GetTextureResource(const RHI::Texture& texture) const;

    // -------------------------------------------------------------------------
    // Multi-volume DDGI / arbitrary descriptor-table-fill helpers.
    //
    // Callers (currently DDGIVolumeManager) own a DescriptorAllocation of N
    // contiguous slots and need to (re-)write individual SRVs into chosen
    // offsets — for example, "slot 2 of the depth-atlas table now points at
    // this newly-allocated R16G16F texture" — without re-allocating the table.
    //
    // All four helpers write to a CPU-side handle into the shader-visible GPU
    // heap (DescriptorAllocation::GetGpuCpuHandle() + slot * descSize). The
    // null variants are required so unallocated DDGI slots still hold valid
    // descriptors (the shader's `vi < ddgiVolumeCount` loop bound prevents the
    // actual reads, but D3D12 validation flags any uninitialised SRV access).
    // -------------------------------------------------------------------------
    void CreateStructuredBufferSRVAtCpu(const RHI::GPUBuffer& buf,
                                        D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void CreateTexture2DSRVAtCpu(const RHI::Texture& tex,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void CreateNullStructuredBufferSRVAtCpu(uint32_t stride,
                                            D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void CreateNullTexture2DSRVAtCpu(DXGI_FORMAT format,
                                     D3D12_CPU_DESCRIPTOR_HANDLE dst) const;

private:
    // Internal swap-chain setup
    void LoadPipeline(HWND hWnd, UINT width, UINT height);
    void LoadAssets();

    // HDR offscreen render target helpers
    void CreateHdrRenderTarget(UINT width, UINT height);
    void ReleaseHdrRenderTarget();

    // Resize helpers
    void ResizeSwapChainAndRtv(UINT width, UINT height);
    void WaitForPreviousFrame();

    // Upload helper: synchronously copies CPU data into a DEFAULT buffer/texture
    // via an UPLOAD staging buffer; flushes and waits for the GPU.
    void FlushUploadAndWait();

    // Release every resource / descriptor / pool slot queued in m_deferredRelease[idx].
    // Caller MUST guarantee the GPU has finished all work that referenced them
    // (pipelined wait in BeginFrame; full drain in WaitForPreviousFrame).
    void ProcessDeferredReleases(uint32_t idx);
    // Shortcut: drain every slot (used after WaitForPreviousFrame fully idles the GPU).
    void ProcessAllDeferredReleases();

    // Drain entries from m_fenceKeyedReleases whose queue fence has reached
    // their target value. Called from BeginFrame after the slot-based drain.
    void ProcessFenceKeyedReleases();

    // Resolve the pool entry for a CommandList handle
    CommandList_DX12& GetPoolEntry(RHI::CommandList cmd);
    const CommandList_DX12& GetPoolEntry(RHI::CommandList cmd) const;

    // DX12 translation helpers
    static DXGI_FORMAT             ToDxgiFormat(RHI::Format f);
    static D3D12_RESOURCE_STATES   ToD3D12ResourceState(RHI::ResourceState s);
    static D3D12_FILTER            ToD3D12Filter(RHI::Filter f);
    static D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(RHI::TextureAddressMode m);
    static D3D12_COMPARISON_FUNC   ToD3D12ComparisonFunc(RHI::ComparisonFunc f);
    static D3D12_BLEND             ToD3D12Blend(RHI::Blend b);
    static D3D12_BLEND_OP          ToD3D12BlendOp(RHI::BlendOp op);
    static D3D12_FILL_MODE         ToD3D12FillMode(RHI::FillMode m);
    static D3D12_CULL_MODE         ToD3D12CullMode(RHI::CullMode m);
    static D3D12_RESOURCE_FLAGS    ToD3D12ResourceFlags(RHI::BindFlag flags);
    static D3D12_HEAP_TYPE         ToD3D12HeapType(RHI::Usage usage);
    static D3D12_PRIMITIVE_TOPOLOGY ToD3D12Topology(RHI::PrimitiveTopology t);
    static D3D12_COMMAND_LIST_TYPE  ToD3D12CommandListType(RHI::QUEUE_TYPE q);

    // Builds the shared root signature used by all PSOs
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateDefaultRootSignature();

    // Builds the compute root signature for post-processing passes
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateComputeRootSignature();

private:
    HWND     m_hWnd  { nullptr };
    uint32_t m_width { 0 };
    uint32_t m_height{ 0 };

    // Core DX12 objects
    // Declaration order determines reverse-destruction order.
    // Required DXGI release sequence: renderTargets → swapChain → queues → device → factory.
    Microsoft::WRL::ComPtr<IDXGIFactory6>             m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
    // One command queue per queue type: [GRAPHICS=0, COMPUTE=1, COPY=2]
    // Declared before swapChain so queues are destroyed AFTER the swap chain.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_queues[3];
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>&       m_commandQueue{ m_queues[0] }; // alias for existing code
    // Declared after queues so swapChain is destroyed BEFORE queues.
    Microsoft::WRL::ComPtr<IDXGISwapChain3>           m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_scRTVHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_renderTargets[FrameCount];
    // CPU-GPU sync fence (graphics queue only, frame pacing)
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;
    HANDLE                                            m_fenceEvent { nullptr };
    uint64_t                                          m_fenceValue { 0 };
    uint32_t                                          m_frameIndex { 0 };
    // Set true by WaitForNextFrameSlot, cleared by EndFrame. BeginFrame uses
    // it to skip a duplicate slot-fence wait when the caller already prepared
    // the slot early (so Animation phase can safely write per-frame upload
    // buffers before BeginFrame). Idempotent within a single frame.
    bool                                              m_frameSlotPrepared { false };
    bool                                              m_tearingSupported { false };
    bool                                              m_meshShaderSupported { false };
    bool                                              m_dxrSupported        { false };
    // Ring buffer of per-backbuffer fence values enabling FrameCount-deep CPU/GPU
    // pipelining. EndFrame writes the just-signaled fv into [m_frameIndex]; next
    // BeginFrame waits on the entry corresponding to the recycled backbuffer
    // (which is FrameCount frames old).
    uint64_t                                          m_frameFenceValues[FrameCount] = {};
    uint64_t                                          m_frameQueueFenceValues[FrameCount][3] = {};

    // Deferred release: Destroy{Buffer,Texture} / ReleaseHdrRenderTarget /
    // FreeDescriptorTable queue their GPU resources + descriptors here instead
    // of freeing them immediately. BeginFrame drains the slot for the recycled
    // backbuffer after its pipelined fence wait — so everything here is
    // guaranteed GPU-safe to release.
    struct DeferredReleaseList
    {
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
        std::vector<DescriptorAllocation>                   descriptors;
        std::vector<uint32_t>                               bufferSlots;
        std::vector<uint32_t>                               textureSlots;
    };
    DeferredReleaseList                               m_deferredRelease[FrameCount];

    // Fence-keyed deferred-release queue. Each entry holds a queue index +
    // fence value the GPU must reach before the resource is safe to free.
    // Drained from BeginFrame after the slot-based queue, removing entries
    // whose fence value <= m_queueFences[queueIndex]->GetCompletedValue().
    // Used by DDGI's async-compute BLAS pipeline so resources can outlive
    // FrameCount without the WaitForPreviousFrame blunt stall.
    struct FenceKeyedRelease
    {
        uint32_t                                            queueIndex;
        uint64_t                                            fenceValue;
        Microsoft::WRL::ComPtr<ID3D12Resource>              resource;
    };
    std::deque<FenceKeyedRelease>                     m_fenceKeyedReleases;

    std::mutex                                        m_deferredMutex;
public:
    bool     vsyncEnabled = true; // false = uncapped FPS with ALLOW_TEARING
private:
    uint32_t                                          m_rtvDescriptorSize{ 0 };
    // Per-queue GPU-GPU cross-queue synchronization fences
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_queueFences[3];
    uint64_t                                          m_queueFenceValues[3]{ 0, 0, 0 };

    // Upload-only command list (used by CreateBuffer / CreateTexture staging copies)
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>      m_uploadAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>   m_commandList;

    // Batch-upload scope depth. When > 0, CreateBuffer(DEFAULT, initialData)
    // skips the per-buffer FlushUploadAndWait() and instead records copies
    // onto the upload CL. EndBufferUploadBatch() flushes once at scope exit.
    // Staging buffers created during the batch are parked in m_batchStagingKeepAlive
    // so they live until the GPU copy completes.
    //
    // Auto-flush thresholds prevent OOM when a batch accumulates thousands of
    // tiny buffers (e.g. legacy per-material .imsh loads). Once *either*
    // threshold is hit, we mid-batch flush + wait + clear staging, then keep
    // the scope open for subsequent CreateBuffer calls. Net cost vs. per-
    // buffer flushing: ~1 wait per 64 MB / 512 buffers instead of ~1 per
    // buffer, which is still a >100× reduction in fence roundtrips.
    uint32_t                                             m_batchUploadDepth = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>  m_batchStagingKeepAlive;
    uint64_t                                             m_batchStagingBytes = 0;
    static constexpr uint64_t kBatchFlushBytes  = 64ull * 1024 * 1024;  // 64 MB
    static constexpr uint32_t kBatchFlushCount  = 512;
    void                                                 FlushBatchStagingIfNeeded();

    // Descriptor heap allocators
    DescriptorHeapAllocator m_rtvAllocator;
    DescriptorHeapAllocator m_cbvSrvUavAllocator;
    DescriptorHeapAllocator m_dsvAllocator;
    DescriptorHeapAllocator m_samplerAllocator;

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT     m_scissorRect{};

    // HDR offscreen render target
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hdrRenderTarget;
    DescriptorAllocation                   m_hdrRtvAllocation;
    DescriptorAllocation                   m_hdrSrvAllocation;
    DescriptorAllocation                   m_hdrUavAllocation; // for SSR composite
    D3D12_RESOURCE_STATES                  m_hdrResourceState{ D3D12_RESOURCE_STATE_RENDER_TARGET };
    uint32_t                               m_hdrWidth { 0 };
    uint32_t                               m_hdrHeight{ 0 };

    // Command list pool — grows dynamically via BlockAllocator; pointers are stable.
    // m_commandListCount is reset to 0 each frame; entries are reused, not freed.
    // m_commandLists is pre-reserved (64 slots) in the constructor so that concurrent
    // push_back calls from BeginCommandList never reallocate while render workers
    // read existing entries via GetPoolEntry (no lock on the read path).
    Allocator::BlockAllocator<CommandList_DX12, 64> m_cmdAllocator;
    std::vector<CommandList_DX12*>              m_commandLists;
    std::atomic<uint32_t>                       m_commandListCount{ 0 };
    std::mutex                                  m_commandListMutex;

    // Resource pools — index == GPUResource::handle_id
    // Pre-reserved to prevent reallocation while other threads hold pool references.
    // Guarded by m_resourceMutex for concurrent Create*/Destroy* calls.
    //
    // m_psoPool uses std::deque of std::unique_ptr<PipelineState_DX12>:
    // Bistro's material permutation count (~1-2k PSOs per run) exceeded the
    // old reserve(256), and once RenderGraph::Execute parallelised pass
    // recording, a realloc in one worker's CreatePipelineState call
    // invalidated the pool references held by other workers'
    // BindPipelineState → AV in D3D12Core.dll. unique_ptr keeps the wrapped
    // PSO at a stable heap address regardless of deque/vector resizing, and
    // deque avoids ever relocating the unique_ptr cells themselves. The
    // forward-declared struct works here because unique_ptr only requires a
    // complete type at destructor instantiation, and ~GraphicsDX12 is
    // defined in GraphicsDX12.cpp where the full struct is visible.
    std::vector<struct GPUBuffer_DX12>                   m_bufferPool;
    std::vector<struct Texture_DX12>                     m_texturePool;
    std::vector<struct Shader_DX12>                      m_shaderPool;
    std::deque<std::unique_ptr<struct PipelineState_DX12>> m_psoPool;
    // Sampler pool: each entry is a live DescriptorAllocation; index == descriptor index.
    std::vector<DescriptorAllocation>      m_samplerPool;
    // Persistent descriptor tables (e.g. shadow cascade SRV tables), keyed by GPU handle.
    std::unordered_map<uint64_t, DescriptorAllocation> m_descriptorTablePool;
    // Free-lists: slots from Destroy* calls that can be reused by the next Create*.
    std::vector<uint32_t>                  m_bufferFreeList;
    std::vector<uint32_t>                  m_textureFreeList;
    std::mutex                             m_resourceMutex;

    // Shared root signature — created once in LoadAssets(), reused by every PSO.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_defaultRootSignature;

    // Compute root signature — created once in LoadAssets(), reused by all compute PSOs.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSignature;

    // Command signature for ExecuteIndirect (root constants + DrawInstanced).
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_indirectCommandSignature;

public:
    // Bindless texture table size — exposed publicly so passes with their own
    // root signatures (DDGIPass, etc.) can size their bindless ranges to match
    // without inlining a magic number.
    //
    // Sized for character-heavy AA: PBR materials carry 5-7 textures each
    // (albedo / normal / orm / emissive / mask / detail), so 200 unique
    // materials = ~1400 textures; add IBL probes, atlases, LUTs, UI sprites
    // and a mid-size game easily reaches ~6-10k resident. 16k leaves comfortable
    // headroom without needing aggressive streaming evictions. Must match
    // g_AllTextures[N] array sizes in DDGIRayTrace.cs.hlsl (other shaders
    // use unbounded `[]` and don't need updating).
    static constexpr uint32_t kMaxBindlessTextures = 16384;

    D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessTextureTableHandle() const { return m_bindlessTexTable.GetGpuHandle(); }
private:
    // Bindless texture table: contiguous GPU-visible SRV block.
    // Index = texture pool handle_id. Bound at root param kBindlessTexSlot (space2).
    // GPU-heap allocation for kMaxBindlessTextures SRVs. Slot index equals
    // m_texturePool's handle_id, so recycling is handled by m_textureFreeList
    // and the descriptor at a reused slot gets overwritten by CreateTexture's
    // CopyDescriptorsSimple. No separate bindless allocator / high-water
    // counter needed.
    DescriptorAllocation m_bindlessTexTable;

    // ---- Present blit (non-ImGui fullscreen SRV → swap chain) ---------------
    // Lazily built on first CompositeTextureToSwapChain call. Used by game-mode
    // App::Run to display the tonemapped scene without needing ImGui.
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_presentRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_presentPSO;
    void EnsurePresentPSO();

    // ---- GPU Profiler (timestamp queries) -----------------------------------
public:
    GPUProfiler& GetGPUProfiler() { return m_gpuProfiler; }
private:
    GPUProfiler  m_gpuProfiler;

    // PSO library (optional — null until InitPSOLibrary is called or on unsupported HW)
    Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> m_psoLibrary;
    std::vector<uint8_t>                          m_psoLibraryData; // serialised bytes, kept alive

    // Derive a stable wchar_t name for a PipelineState from its creation desc.
    // Non-static because the fallback path looks up bytecode hashes from
    // m_shaderPool — those give content-addressed names stable across runs,
    // letting the disk PSO library actually reuse entries instead of growing
    // unbounded every launch.
    void ComputePSOName(const RHI::PipelineStateDesc& desc,
                        wchar_t* outName, size_t maxChars) const;

#ifdef _DEBUG
    // D3D12 debug layer message callback cookie (0 = not registered)
    DWORD m_d3d12MessageCallbackCookie{ 0 };
#endif

    // Video decode backend — lazily constructed on first GetVideoBackend().
    // Forward-declared at the top so we don't drag d3d12video.h into the
    // public header; the destructor is defined in GraphicsDX12.cpp where the
    // full VideoDecoderDX12 type is visible.
    std::unique_ptr<RHI::DX12::VideoDecoderDX12> m_videoBackend;
    std::mutex                                   m_videoBackendMutex;
};
