#pragma once

// IVideoDecoder — platform-agnostic GPU video decoder interface.
//
// Mirrors the RHI's IGraphicsDevice pattern: pure-virtual surface that hides
// API-specific objects (D3D12 video device / Vulkan video queue) behind a
// minimal Create / Submit / Destroy contract.
//
// Scope contract — what RHI owns vs. what the caller owns:
//   * RHI owns:    GPU video session, decoder heap, DPB texture allocation,
//                  output NV12 texture, decode command list submission, and
//                  cross-queue synchronisation with the graphics queue.
//   * Caller owns: container demux (mp4/mkv), NALU extraction, SPS/PPS
//                  parsing, slice-header parsing, DPB reference management.
//                  The caller hands over a fully-populated `VideoDecodeArgs`
//                  containing the raw bitstream slice bytes plus the
//                  DXVA-compatible picture parameter struct.
//
// Output format: every decoded frame writes into an NV12 RHI::Texture (two
// planes: R8_UNORM Y + R8G8_UNORM UV). Caller can:
//   1. Sample the two planes in a fullscreen PS doing YUV→RGB conversion
//      (preferred — fits the existing PostProcessing pass model), or
//   2. Run a separate compute shader to write an interleaved RGBA texture.
//
// Threading: CreateVideoDecoder / Destroy is main-thread only (touches the
// resource pool). SubmitDecode is safe from any thread once the decoder
// handle is valid; the backend serialises on the video queue internally.
//
// Standard Annex-B Network Abstraction Layer Units (NALUs) carry the raw
// bitstream — the caller MUST strip the 0x000001 / 0x00000001 start codes
// before passing the slice bytes. D3D12 video decode wants the EBSP payload
// directly with the 3-byte NAL-unit header stripped per slice.

#include "Graphics/GraphicsStruct.h"

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
struct _DXVA_PicParams_H264;
struct _DXVA_PicParams_HEVC;
struct _DXVA_Slice_H264_Short;
struct _DXVA_Slice_HEVC_Short;
struct _DXVA_Qmatrix_H264;
struct _DXVA_Qmatrix_HEVC;
#endif

namespace RHI
{
    // ----------------------------------------------------------------------
    // Codec / profile enumeration
    // ----------------------------------------------------------------------

    enum class VideoCodec : uint8_t
    {
        H264,   // AVC — MPEG-4 Part 10
        H265,   // HEVC — MPEG-H Part 2
    };

    // Subset of the codec profiles D3D12 Video accepts. Listing only the
    // profiles found in the wild for game / FMV use (Baseline / Main / High
    // for H.264; Main / Main10 for H.265).
    enum class VideoProfile : uint8_t
    {
        H264_Baseline = 0,
        H264_Main     = 1,
        H264_High     = 2,
        H265_Main     = 10,  // 8-bit
        H265_Main10   = 11,  // 10-bit HDR (yields P010 output, not NV12)
    };

    // ----------------------------------------------------------------------
    // VideoDecoderDesc — frozen at create time, immutable for the decoder's
    // lifetime. Picture / DPB resources are sized off these values.
    // ----------------------------------------------------------------------
    struct VideoDecoderDesc
    {
        VideoCodec   codec        = VideoCodec::H264;
        VideoProfile profile      = VideoProfile::H264_High;
        uint32_t     maxWidth     = 1920;   // mb-aligned upper bound
        uint32_t     maxHeight    = 1080;
        uint32_t     bitDepth     = 8;      // 8 for NV12, 10 for P010
        // DPB (Decoded Picture Buffer) — number of reference slots the codec
        // may need to keep alive. H.264 Level 4.1 / 1080p needs 4-5;
        // headroom of 17 (DPB_MAX_PIC_COUNT_H264) is conservative + safe.
        uint32_t     dpbSlotCount = 17;
        // Some shipping titles concatenate multiple SPS/PPS streams in one
        // playlist; setting this true tells the backend to skip caching that
        // depends on parameter-set IDs being stable across frames.
        bool         allowProfileSwitching = false;
        // UTF-8 debug name forwarded to ID3D12Resource::SetName so PIX /
        // RenderDoc captures show a meaningful identifier. Optional.
        const char*  debugName    = nullptr;
    };

    // ----------------------------------------------------------------------
    // Per-frame decode arguments
    //
    //   bitstream       — pointer to the raw NALU EBSP bytes for ONE access
    //                     unit (one picture). Start codes already stripped.
    //   bitstreamBytes  — total bytes in the buffer.
    //   sliceControl    — array of slice headers (DXVA_Slice_*_Short). Each
    //                     entry has SliceBytesInBuffer + SliceOffset to mark
    //                     where each slice starts inside `bitstream`.
    //   sliceCount      — number of entries in `sliceControl`.
    //   pictureParams   — pointer to DXVA_PicParams_H264 or DXVA_PicParams_HEVC
    //                     depending on codec. The struct is reinterpreted by
    //                     the backend; layout MUST match the DXVA spec exactly.
    //   pictureParamsBytes — sizeof(struct) at the API boundary so a future
    //                     ABI bump doesn't silently corrupt decoding.
    //   quantMatrices   — DXVA_Qmatrix_H264 / DXVA_Qmatrix_HEVC, optional
    //                     (nullptr → use defaults / inferred).
    //   outputTexture   — DPB-marked NV12 RHI::Texture that receives the
    //                     decoded picture. Must outlive the GPU work; the
    //                     backend places it in VIDEO_DECODE_DST → SHADER_RESOURCE
    //                     by itself.
    //   dpbReferences   — array of NV12 textures that the picture references
    //                     (other frames already decoded into the DPB). Order
    //                     and slot indices follow the SPS/PPS / slice header
    //                     reference list as parsed by the caller.
    //   dpbReferenceCount — number of entries in dpbReferences.
    //
    //   frameIndex      — caller-supplied increasing frame number. Backend uses
    //                     it for the per-frame fence value + profiling.
    //
    //   isReferenceFrame — true if this picture is itself going into the DPB
    //                     (most P/I frames). Affects how the backend tracks
    //                     resource state for outputTexture.
    // ----------------------------------------------------------------------
    struct VideoDecodeArgs
    {
        const void*    bitstream            = nullptr;
        size_t         bitstreamBytes       = 0;

        const void*    sliceControl         = nullptr;
        size_t         sliceCount           = 0;

        const void*    pictureParams        = nullptr;
        size_t         pictureParamsBytes   = 0;

        const void*    quantMatrices        = nullptr;
        size_t         quantMatricesBytes   = 0;

        const Texture* outputTexture        = nullptr;

        const Texture* const* dpbReferences = nullptr;
        size_t         dpbReferenceCount    = 0;

        uint64_t       frameIndex           = 0;
        bool           isReferenceFrame     = true;
    };

    // Opaque handle to a backend-owned video decoder. internal_id indexes
    // into the backend's decoder pool (same convention as GPUResource).
    struct VideoDecoder
    {
        uint32_t handle_id = ~0u;
        constexpr bool IsValid() const noexcept { return handle_id != ~0u; }
        void Reset() noexcept { handle_id = ~0u; }
    };

    // ----------------------------------------------------------------------
    // IVideoDecoder
    //
    // Backed by GraphicsDX12::CreateVideoDecoder which lazily QIs
    // ID3D12VideoDevice and stands up the video queue on first use. Null
    // return = HW does not support that codec/profile combination.
    // ----------------------------------------------------------------------
    class IVideoDecoderBackend
    {
    public:
        virtual ~IVideoDecoderBackend() = default;

        // Create the underlying decoder + heap + DPB allocations.
        // Returns false when the HW reports the codec/profile is not
        // supported (CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT)).
        virtual bool CreateVideoDecoder(const VideoDecoderDesc& desc,
                                        VideoDecoder& outDecoder) = 0;

        // Submit one picture's worth of decode work. The video queue
        // signals its own fence on completion; callers that need the
        // graphics queue to wait on the result should call
        // AddDecodeDependency before binding the output texture.
        // Returns false when the args fail validation (mismatched codec,
        // missing output texture, picture params size mismatch, etc.).
        virtual bool DecodeVideoFrame(VideoDecoder decoder,
                                      const VideoDecodeArgs& args) = 0;

        // Insert a GPU wait on the graphics queue: subsequent graphics CLs
        // will block until the video queue has finished decoding frame
        // @p frameIndex of @p decoder. Pairs with isReferenceFrame=true in
        // DecodeVideoFrame so the caller can sample outputTexture safely.
        virtual void AddDecodeDependency(VideoDecoder decoder,
                                         uint64_t     frameIndex,
                                         CommandList  waiter) = 0;

        virtual void DestroyVideoDecoder(VideoDecoder& decoder) = 0;

        // Allocate an NV12 texture suitable for use as DPB slot AND as a
        // shader resource (two-plane SRV). Sets bind_flags = SHADER_RESOURCE
        // and misc_flags |= VIDEO_DECODE so the backend creates the texture
        // on a video-compatible heap with the correct cross-queue layout.
        virtual bool CreateVideoOutputTexture(uint32_t width, uint32_t height,
                                              VideoCodec compatibility,
                                              Texture& outTexture) = 0;
    };

} // namespace RHI
