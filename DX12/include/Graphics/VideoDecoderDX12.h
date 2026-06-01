#pragma once

// VideoDecoderDX12 — D3D12 implementation of IVideoDecoderBackend.
//
// Lifecycle / ownership:
//   * One instance is lazily constructed by GraphicsDX12 on the first
//     GetVideoBackend() call.
//   * On construction the backend QIs ID3D12VideoDevice from the shared
//     ID3D12Device and stands up a dedicated D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE
//     queue + fence.
//   * Per-decoder objects (ID3D12VideoDecoder, ID3D12VideoDecoderHeap,
//     bitstream staging ring, per-slot allocators) live in m_decoderPool
//     indexed by RHI::VideoDecoder::handle_id.
//
// Queue layout:
//   The video queue is separate from the engine's graphics / compute / copy
//   queues. Its fence is GraphicsDX12::m_videoFence (created here, not in
//   the graphics queue array). Callers that need a graphics CL to read the
//   decoded NV12 output use AddDecodeDependency(decoder, frameIndex, gfxCL)
//   which records ID3D12CommandQueue::Wait on the graphics queue against
//   the video fence value associated with `frameIndex`.
//
// Bitstream staging:
//   For simplicity the input bitstream comes from an UPLOAD-heap committed
//   resource that we map-and-memcpy on every DecodeVideoFrame call. D3D12
//   video decode accepts UPLOAD-heap input on every shipping driver. The
//   ring depth (= GraphicsDX12::FrameCount) lets up to FrameCount decodes
//   stay in flight before CPU has to wait.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <wrl.h>
#include <vector>
#include <mutex>

#include "d3d12.h"
#include "d3d12video.h"

#include "Graphics/IVideoDecoder.h"

class GraphicsDX12;

namespace RHI::DX12
{
    // ----------------------------------------------------------------------
    // VideoDecoder_DX12 — per-decoder resource bundle. One entry per
    // m_decoderPool slot; index == RHI::VideoDecoder::handle_id.
    // ----------------------------------------------------------------------
    struct VideoDecoder_DX12
    {
        static constexpr uint32_t kRingDepth = 3; // matches GraphicsDX12::FrameCount

        // Decoder config (kept for state queries — DXVA picture params reference
        // some fields like profile + bit depth).
        RHI::VideoDecoderDesc                            desc{};

        // D3D12 video decode objects (immutable after CreateVideoDecoder).
        Microsoft::WRL::ComPtr<ID3D12VideoDecoder>       decoder;
        Microsoft::WRL::ComPtr<ID3D12VideoDecoderHeap>   heap;

        // Per-slot bitstream staging — round-robin to allow up to kRingDepth
        // in-flight DecodeVideoFrame calls without CPU wait.
        Microsoft::WRL::ComPtr<ID3D12Resource>           bitstreamBuffers[kRingDepth];
        uint64_t                                          bitstreamCapacityBytes = 0;

        // Per-slot DXVA picture-params staging (must live on a GPU-readable
        // resource for D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS).
        Microsoft::WRL::ComPtr<ID3D12Resource>           pictureParamBuffers[kRingDepth];
        uint64_t                                          pictureParamCapacityBytes = 0;

        // Per-slot slice-control + qmatrix staging packed into one buffer.
        Microsoft::WRL::ComPtr<ID3D12Resource>           sliceControlBuffers[kRingDepth];
        uint64_t                                          sliceControlCapacityBytes = 0;

        // Per-slot decode command allocator + list.
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>           allocators[kRingDepth];
        Microsoft::WRL::ComPtr<ID3D12VideoDecodeCommandList>     commandList;

        // Most recently submitted video fence value for each frameIndex
        // bucket — used by AddDecodeDependency to insert the cross-queue wait.
        // Keyed slot = frameIndex % kRingDepth so the caller can refer back to
        // a recent frame without us having to keep an unbounded map.
        uint64_t                                          slotFenceValues[kRingDepth] = {};

        uint32_t                                          nextSlot = 0;

        // 4 MB default — covers 1080p H.264 main profile easily; 4K H.265 can
        // exceed 8 MB on intra-only segments, so the backend grows the buffer
        // on demand when a slice exceeds capacity.
        static constexpr uint64_t kDefaultBitstreamCapacity = 4ull * 1024 * 1024;
    };

    // ----------------------------------------------------------------------
    // VideoDecoderDX12 — IVideoDecoderBackend impl
    // ----------------------------------------------------------------------
    class VideoDecoderDX12 final : public RHI::IVideoDecoderBackend
    {
    public:
        explicit VideoDecoderDX12(GraphicsDX12& gfx);
        ~VideoDecoderDX12() override;

        // Returns true after the constructor successfully QI'd
        // ID3D12VideoDevice + created the video queue. False on older drivers
        // / WARP — callers should fall back to no-video paths.
        bool IsAvailable() const { return m_videoDevice && m_videoQueue; }

        // ---- IVideoDecoderBackend --------------------------------------------
        bool CreateVideoDecoder(const RHI::VideoDecoderDesc& desc,
                                RHI::VideoDecoder& outDecoder) override;

        bool DecodeVideoFrame(RHI::VideoDecoder decoder,
                              const RHI::VideoDecodeArgs& args) override;

        void AddDecodeDependency(RHI::VideoDecoder decoder,
                                 uint64_t          frameIndex,
                                 RHI::CommandList  waiter) override;

        void DestroyVideoDecoder(RHI::VideoDecoder& decoder) override;

        bool CreateVideoOutputTexture(uint32_t width, uint32_t height,
                                      RHI::VideoCodec compatibility,
                                      RHI::Texture& outTexture) override;

        // ---- DX12-only accessors (used by Graphics layer when needed) --------
        ID3D12VideoDevice*    GetVideoDevice() const { return m_videoDevice.Get(); }
        ID3D12CommandQueue*   GetVideoQueue()  const { return m_videoQueue.Get();  }
        uint64_t              GetLastSignaledVideoFence() const { return m_videoFenceValue; }

    private:
        // Helper: pick the DXVA profile GUID matching desc.codec + desc.profile.
        static const GUID& PickProfileGuid(const RHI::VideoDecoderDesc& desc);

        // Helper: grow an UPLOAD-heap staging buffer if @p neededBytes exceeds
        // the current capacity. Reallocates on demand; otherwise no-op.
        void EnsureUploadBuffer(Microsoft::WRL::ComPtr<ID3D12Resource>& buf,
                                uint64_t& capacityBytes,
                                uint64_t  neededBytes) const;

        GraphicsDX12&                                                 m_gfx;
        Microsoft::WRL::ComPtr<ID3D12VideoDevice>                     m_videoDevice;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue>                    m_videoQueue;
        Microsoft::WRL::ComPtr<ID3D12Fence>                           m_videoFence;
        uint64_t                                                      m_videoFenceValue = 0;
        // Reusable wait event for the DecodeVideoFrame ring-saturation stall —
        // created once instead of CreateEventW/CloseHandle per wait. Used only
        // from the decode thread (DecodeVideoFrame takes no lock); the teardown
        // paths keep their own local events to avoid cross-thread sharing.
        HANDLE                                                        m_videoFenceEvent = nullptr;
        std::vector<VideoDecoder_DX12>                                m_decoderPool;
        std::vector<uint32_t>                                         m_decoderFreeList;
        std::mutex                                                    m_decoderMutex;
    };

} // namespace RHI::DX12
