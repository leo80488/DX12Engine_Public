#include "Graphics/VideoDecoderDX12.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "System/Log.h"

#include <initguid.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxva.h>

// DXVA decode profile GUIDs (subset). Defined here to avoid pulling in
// the entire DXVA header chain into VideoDecoderDX12.h. The values are the
// standard ones published by Microsoft for D3D12 video decode.
DEFINE_GUID(D3D12_VIDEO_DECODE_PROFILE_H264,
    0x1b81be68, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);
DEFINE_GUID(D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN,
    0x5b11d51b, 0x2f4c, 0x4452, 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0);
DEFINE_GUID(D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10,
    0x107af0e0, 0xef1a, 0x4d19, 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13);

#pragma comment(lib, "d3d12.lib")

using Microsoft::WRL::ComPtr;

namespace RHI::DX12
{

// ===========================================================================
// Construction — QI the video device + spin up the dedicated video queue.
// ===========================================================================

VideoDecoderDX12::VideoDecoderDX12(GraphicsDX12& gfx)
    : m_gfx(gfx)
{
    ID3D12Device* device = m_gfx.GetDevice();
    if (!device)
    {
        LOG_ERROR("VideoDecoderDX12: GraphicsDX12::GetDevice returned null");
        return;
    }
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&m_videoDevice))))
    {
        LOG_WARNING("VideoDecoderDX12: ID3D12VideoDevice not exposed by driver "
                    "(software fallback / very old GPU). Video decode disabled.");
        return;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_videoQueue))))
    {
        LOG_ERROR("VideoDecoderDX12: CreateCommandQueue(VIDEO_DECODE) failed");
        m_videoDevice.Reset();
        return;
    }
    m_videoQueue->SetName(L"VideoDecoderDX12::m_videoQueue");

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_videoFence))))
    {
        LOG_ERROR("VideoDecoderDX12: CreateFence failed");
        m_videoQueue.Reset();
        m_videoDevice.Reset();
        return;
    }
    m_videoFence->SetName(L"VideoDecoderDX12::m_videoFence");
    m_videoFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    LOG_SUCCESS("VideoDecoderDX12: video device + queue ready");
}

VideoDecoderDX12::~VideoDecoderDX12()
{
    // No explicit wait — GraphicsDX12::WaitIdleAndReleaseDeferred is called
    // from ~GraphicsDX12 before this is destroyed, draining the graphics
    // queue. For the video queue we additionally CPU-block on the last
    // signaled fence so per-decoder ComPtrs free on a quiet GPU.
    if (m_videoQueue && m_videoFence && m_videoFenceValue > 0)
    {
        const HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (evt)
        {
            if (SUCCEEDED(m_videoFence->SetEventOnCompletion(m_videoFenceValue, evt)))
                WaitForSingleObject(evt, INFINITE);
            CloseHandle(evt);
        }
    }
    if (m_videoFenceEvent) { CloseHandle(m_videoFenceEvent); m_videoFenceEvent = nullptr; }
}

// ===========================================================================
// Profile GUID picker
// ===========================================================================

const GUID& VideoDecoderDX12::PickProfileGuid(const RHI::VideoDecoderDesc& desc)
{
    switch (desc.codec)
    {
    case RHI::VideoCodec::H264:
        return D3D12_VIDEO_DECODE_PROFILE_H264;
    case RHI::VideoCodec::H265:
        return (desc.profile == RHI::VideoProfile::H265_Main10)
            ? D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10
            : D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
    }
    return D3D12_VIDEO_DECODE_PROFILE_H264;
}

// ===========================================================================
// EnsureUploadBuffer — lazy grow of an UPLOAD-heap committed buffer.
// ===========================================================================

void VideoDecoderDX12::EnsureUploadBuffer(ComPtr<ID3D12Resource>& buf,
                                          uint64_t& capacityBytes,
                                          uint64_t  neededBytes) const
{
    if (buf && capacityBytes >= neededBytes) return;

    // Round up to 64 KB to keep reallocations rare.
    constexpr uint64_t kChunk = 65536ull;
    const uint64_t newCap = ((neededBytes + kChunk - 1) / kChunk) * kChunk;

    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC   rd{};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width              = newCap;
    rd.Height             = 1;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.SampleDesc.Count   = 1;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> fresh;
    if (FAILED(m_gfx.GetDevice()->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&fresh))))
    {
        LOG_ERROR("VideoDecoderDX12::EnsureUploadBuffer: CreateCommittedResource failed (%llu bytes)", newCap);
        return;
    }
    buf           = std::move(fresh);
    capacityBytes = newCap;
}

// ===========================================================================
// CreateVideoDecoder — query feature support, then allocate ID3D12VideoDecoder
// + ID3D12VideoDecoderHeap + per-slot staging.
// ===========================================================================

bool VideoDecoderDX12::CreateVideoDecoder(const RHI::VideoDecoderDesc& desc,
                                          RHI::VideoDecoder& outDecoder)
{
    if (!IsAvailable())
    {
        LOG_ERROR("CreateVideoDecoder called on unavailable video backend");
        return false;
    }

    const GUID& profile = PickProfileGuid(desc);

    // Step 1: Feature support query — confirms the GPU can decode this codec.
    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT fs{};
    fs.NodeIndex          = 0;
    fs.Configuration.DecodeProfile = profile;
    fs.Configuration.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
    fs.Configuration.InterlaceType       = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
    fs.Width              = desc.maxWidth;
    fs.Height             = desc.maxHeight;
    fs.DecodeFormat       = (desc.bitDepth >= 10) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    fs.FrameRate          = { 60, 1 };
    fs.BitRate            = 0;

    if (FAILED(m_videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &fs, sizeof(fs))))
    {
        LOG_ERROR("CreateVideoDecoder: CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT) failed");
        return false;
    }
    if (!(fs.DecodeTier >= D3D12_VIDEO_DECODE_TIER_1)
        || !(fs.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED))
    {
        LOG_ERROR("CreateVideoDecoder: profile not supported on this GPU "
                  "(codec=%u maxWxH=%ux%u)",
                  (unsigned)desc.codec, desc.maxWidth, desc.maxHeight);
        return false;
    }

    // Step 2: Build the per-decoder entry.
    VideoDecoder_DX12 entry;
    entry.desc = desc;

    D3D12_VIDEO_DECODER_DESC vdd{};
    vdd.NodeMask                          = 0;
    vdd.Configuration.DecodeProfile       = profile;
    vdd.Configuration.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
    vdd.Configuration.InterlaceType       = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;

    if (FAILED(m_videoDevice->CreateVideoDecoder(&vdd, IID_PPV_ARGS(&entry.decoder))))
    {
        LOG_ERROR("CreateVideoDecoder: CreateVideoDecoder failed");
        return false;
    }

    D3D12_VIDEO_DECODER_HEAP_DESC hd{};
    hd.NodeMask                 = 0;
    hd.Configuration            = vdd.Configuration;
    hd.DecodeWidth              = desc.maxWidth;
    hd.DecodeHeight             = desc.maxHeight;
    hd.Format                   = fs.DecodeFormat;
    hd.FrameRate                = fs.FrameRate;
    hd.BitRate                  = 0;
    hd.MaxDecodePictureBufferCount = desc.dpbSlotCount;

    if (FAILED(m_videoDevice->CreateVideoDecoderHeap(&hd, IID_PPV_ARGS(&entry.heap))))
    {
        LOG_ERROR("CreateVideoDecoder: CreateVideoDecoderHeap failed");
        return false;
    }

    // Step 3: Per-slot command allocators + the (shared, reused) command list.
    ID3D12Device* dev = m_gfx.GetDevice();
    for (uint32_t i = 0; i < VideoDecoder_DX12::kRingDepth; ++i)
    {
        if (FAILED(dev->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE,
                IID_PPV_ARGS(&entry.allocators[i]))))
        {
            LOG_ERROR("CreateVideoDecoder: CreateCommandAllocator(video) failed slot %u", i);
            return false;
        }

        // Pre-allocate the default-sized bitstream / picture-param / slice
        // staging buffers; they'll grow on demand inside DecodeVideoFrame.
        EnsureUploadBuffer(entry.bitstreamBuffers[i], entry.bitstreamCapacityBytes,
                           VideoDecoder_DX12::kDefaultBitstreamCapacity);
        EnsureUploadBuffer(entry.pictureParamBuffers[i], entry.pictureParamCapacityBytes,
                           64 * 1024);
        EnsureUploadBuffer(entry.sliceControlBuffers[i], entry.sliceControlCapacityBytes,
                           64 * 1024);
    }

    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE,
                                       entry.allocators[0].Get(), nullptr,
                                       IID_PPV_ARGS(&entry.commandList))))
    {
        LOG_ERROR("CreateVideoDecoder: CreateCommandList(VIDEO_DECODE) failed");
        return false;
    }
    entry.commandList->Close();

    // Step 4: park into the pool.
    std::lock_guard<std::mutex> lock(m_decoderMutex);
    if (!m_decoderFreeList.empty())
    {
        outDecoder.handle_id = m_decoderFreeList.back();
        m_decoderFreeList.pop_back();
        m_decoderPool[outDecoder.handle_id] = std::move(entry);
    }
    else
    {
        outDecoder.handle_id = static_cast<uint32_t>(m_decoderPool.size());
        m_decoderPool.push_back(std::move(entry));
    }

    LOG_SUCCESS("VideoDecoderDX12: created decoder id=%u codec=%u %ux%u dpb=%u",
                outDecoder.handle_id, (unsigned)desc.codec,
                desc.maxWidth, desc.maxHeight, desc.dpbSlotCount);
    return true;
}

// ===========================================================================
// DecodeVideoFrame — stage args into the ring, then submit one DecodeFrame.
// ===========================================================================

bool VideoDecoderDX12::DecodeVideoFrame(RHI::VideoDecoder decoder,
                                        const RHI::VideoDecodeArgs& args)
{
    if (!IsAvailable() || !decoder.IsValid()) return false;
    if (decoder.handle_id >= m_decoderPool.size())
    {
        LOG_ERROR("DecodeVideoFrame: handle out of range");
        return false;
    }
    auto& entry = m_decoderPool[decoder.handle_id];
    if (!entry.decoder || !args.bitstream || args.bitstreamBytes == 0
        || !args.pictureParams || args.pictureParamsBytes == 0
        || !args.outputTexture || !args.outputTexture->IsValid())
    {
        LOG_ERROR("DecodeVideoFrame: invalid arguments");
        return false;
    }

    const uint32_t slot = (entry.nextSlot++) % VideoDecoder_DX12::kRingDepth;

    // CPU-block until this slot's last submitted fence has retired, so we can
    // safely overwrite the staging buffers + reset the allocator.
    if (entry.slotFenceValues[slot] > 0
        && m_videoFence->GetCompletedValue() < entry.slotFenceValues[slot])
    {
        // Reuse the cached event (decode-thread only) instead of allocating a
        // Win32 event per ring-saturation stall. Auto-reset, so it's ready for
        // the next wait. Fall back to a one-shot event if the cache is null.
        if (m_videoFenceEvent)
        {
            m_videoFence->SetEventOnCompletion(entry.slotFenceValues[slot], m_videoFenceEvent);
            WaitForSingleObject(m_videoFenceEvent, INFINITE);
        }
        else if (const HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr))
        {
            m_videoFence->SetEventOnCompletion(entry.slotFenceValues[slot], evt);
            WaitForSingleObject(evt, INFINITE);
            CloseHandle(evt);
        }
    }

    // ---- Grow staging buffers on demand ---------------------------------
    EnsureUploadBuffer(entry.bitstreamBuffers[slot],   entry.bitstreamCapacityBytes,   args.bitstreamBytes);
    EnsureUploadBuffer(entry.pictureParamBuffers[slot], entry.pictureParamCapacityBytes, args.pictureParamsBytes);
    const size_t sliceTotalBytes =
        (args.sliceControl ? args.sliceCount * 256u : 0) + // worst-case 256 B per slice header
        args.quantMatricesBytes;
    EnsureUploadBuffer(entry.sliceControlBuffers[slot], entry.sliceControlCapacityBytes,
                       (std::max)(sliceTotalBytes, (size_t)4096u));

    // ---- Copy CPU data into the staging buffers --------------------------
    auto uploadCopy = [](ID3D12Resource* dst, const void* src, size_t bytes)
    {
        if (!dst || !src || bytes == 0) return;
        void* p = nullptr;
        D3D12_RANGE rr{ 0, 0 };
        if (SUCCEEDED(dst->Map(0, &rr, &p)) && p)
        {
            std::memcpy(p, src, bytes);
            dst->Unmap(0, nullptr);
        }
    };
    uploadCopy(entry.bitstreamBuffers[slot].Get(),    args.bitstream,         args.bitstreamBytes);
    uploadCopy(entry.pictureParamBuffers[slot].Get(), args.pictureParams,     args.pictureParamsBytes);
    if (args.sliceControl && args.sliceCount > 0)
    {
        // DXVA slice headers are tightly packed; copy the whole array.
        // sizeof one entry varies by codec (DXVA_Slice_H264_Short is 12 bytes,
        // DXVA_Slice_HEVC_Short is 12 bytes too). Caller passes the byte total
        // implicitly via sliceCount * sizeof(short slice header).
        const size_t bytesPerSlice = sizeof(DXVA_Slice_H264_Short); // both codecs share size
        uploadCopy(entry.sliceControlBuffers[slot].Get(),
                   args.sliceControl, args.sliceCount * bytesPerSlice);
    }

    // ---- Reset command list, transition output → VIDEO_DECODE_WRITE ------
    if (FAILED(entry.allocators[slot]->Reset()))
    {
        LOG_ERROR("DecodeVideoFrame: allocator->Reset failed");
        return false;
    }
    if (FAILED(entry.commandList->Reset(entry.allocators[slot].Get())))
    {
        LOG_ERROR("DecodeVideoFrame: commandList->Reset failed");
        return false;
    }

    ID3D12Resource* outRes = m_gfx.GetTextureResource(*args.outputTexture);
    if (!outRes)
    {
        LOG_ERROR("DecodeVideoFrame: output texture has no D3D12 resource");
        entry.commandList->Close();
        return false;
    }

    // Output texture: VIDEO_DECODE_WRITE before DecodeFrame, then we leave it
    // in WRITE state. Caller transitions to SHADER_RESOURCE on the graphics
    // queue after AddDecodeDependency.
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = outRes;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        entry.commandList->ResourceBarrier(1, &b);
    }

    // ---- Assemble D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS -------------
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in{};
    in.NumFrameArguments = 0;

    // Picture params
    {
        auto& fa = in.FrameArguments[in.NumFrameArguments++];
        fa.Type  = D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS;
        fa.Size  = static_cast<UINT>(args.pictureParamsBytes);
        // The driver reads pData by CPU pointer for ARGUMENT_TYPE_PICTURE_PARAMETERS
        // (it is NOT a GPU buffer). Hand it the raw struct.
        fa.pData = const_cast<void*>(args.pictureParams);
    }
    // Slice control
    if (args.sliceControl && args.sliceCount > 0)
    {
        auto& fa = in.FrameArguments[in.NumFrameArguments++];
        fa.Type  = D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL;
        fa.Size  = static_cast<UINT>(args.sliceCount * sizeof(DXVA_Slice_H264_Short));
        fa.pData = const_cast<void*>(args.sliceControl);
    }
    // Optional inverse-quantization matrix
    if (args.quantMatrices && args.quantMatricesBytes > 0)
    {
        auto& fa = in.FrameArguments[in.NumFrameArguments++];
        fa.Type  = D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX;
        fa.Size  = static_cast<UINT>(args.quantMatricesBytes);
        fa.pData = const_cast<void*>(args.quantMatrices);
    }

    in.CompressedBitstream.pBuffer = entry.bitstreamBuffers[slot].Get();
    in.CompressedBitstream.Offset  = 0;
    in.CompressedBitstream.Size    = args.bitstreamBytes;
    in.pHeap                       = entry.heap.Get();

    // DPB reference frames — resolve RHI handles to ID3D12Resource pointers.
    // Stack allocate up to 17 (DPB_MAX_PIC_COUNT_H264). HEVC limit is 16.
    constexpr UINT kMaxRefs = 17;
    ID3D12Resource*           refResPtrs[kMaxRefs] = {};
    UINT                      refSubres[kMaxRefs]  = {};
    ID3D12VideoDecoderHeap*   refHeaps[kMaxRefs]   = {};
    const UINT refCount = (args.dpbReferenceCount < kMaxRefs)
                        ? (UINT)args.dpbReferenceCount : kMaxRefs;
    for (UINT i = 0; i < refCount; ++i)
    {
        const RHI::Texture* refTex = args.dpbReferences[i];
        refResPtrs[i] = refTex ? m_gfx.GetTextureResource(*refTex) : nullptr;
        refSubres[i]  = 0;
        refHeaps[i]   = entry.heap.Get();
    }
    in.ReferenceFrames.NumTexture2Ds = refCount;
    in.ReferenceFrames.ppTexture2Ds  = refResPtrs;
    in.ReferenceFrames.pSubresources = refSubres;
    in.ReferenceFrames.ppHeaps       = refHeaps;

    // ---- Assemble D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS ------------
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS out{};
    out.pOutputTexture2D     = outRes;
    out.OutputSubresource    = 0;
    out.ConversionArguments  = {};   // no built-in YUV conversion

    // ---- DecodeFrame + transition output back to COMMON for cross-queue use
    entry.commandList->DecodeFrame(entry.decoder.Get(), &out, &in);

    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = outRes;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        entry.commandList->ResourceBarrier(1, &b);
    }

    if (FAILED(entry.commandList->Close()))
    {
        LOG_ERROR("DecodeVideoFrame: commandList->Close failed");
        return false;
    }

    // ---- Submit on the video queue, signal the per-frame fence ----------
    ID3D12CommandList* lists[] = { entry.commandList.Get() };
    m_videoQueue->ExecuteCommandLists(1, lists);

    const uint64_t fv = ++m_videoFenceValue;
    if (FAILED(m_videoQueue->Signal(m_videoFence.Get(), fv)))
    {
        LOG_ERROR("DecodeVideoFrame: video queue signal failed");
        return false;
    }
    entry.slotFenceValues[slot] = fv;
    return true;
}

// ===========================================================================
// AddDecodeDependency — graphics queue Wait on the video fence.
// ===========================================================================

void VideoDecoderDX12::AddDecodeDependency(RHI::VideoDecoder decoder,
                                            uint64_t          /*frameIndex*/,
                                            RHI::CommandList  waiter)
{
    if (!IsAvailable() || !decoder.IsValid()) return;
    if (decoder.handle_id >= m_decoderPool.size()) return;
    const auto& entry = m_decoderPool[decoder.handle_id];

    // Pick the most recent submission for this decoder regardless of which
    // frameIndex slot the caller asked about — the slot fence values are
    // monotone, so taking max is safe and avoids exposing the ring depth.
    uint64_t maxFv = 0;
    for (uint64_t fv : entry.slotFenceValues) maxFv = (std::max)(maxFv, fv);
    if (maxFv == 0) return;

    // The graphics queue must wait for the video decode to finish before the
    // caller's command list reads the NV12 output. We insert the wait on the
    // queue (not the CL), which is sufficient for any subsequently executed
    // graphics CL — including @p waiter.
    if (auto* gfxQueue = m_gfx.GetGraphicsQueue())
        gfxQueue->Wait(m_videoFence.Get(), maxFv);
    (void)waiter;
}

// ===========================================================================
// DestroyVideoDecoder
// ===========================================================================

void VideoDecoderDX12::DestroyVideoDecoder(RHI::VideoDecoder& decoder)
{
    if (!decoder.IsValid()) return;
    std::lock_guard<std::mutex> lock(m_decoderMutex);
    if (decoder.handle_id >= m_decoderPool.size()) { decoder.Reset(); return; }

    auto& entry = m_decoderPool[decoder.handle_id];

    // Wait until every in-flight decode on this decoder retires before we
    // drop the ComPtrs. Cheap because video queue is short-pipeline.
    uint64_t maxFv = 0;
    for (uint64_t fv : entry.slotFenceValues) maxFv = (std::max)(maxFv, fv);
    if (maxFv > 0 && m_videoFence && m_videoFence->GetCompletedValue() < maxFv)
    {
        const HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (evt)
        {
            m_videoFence->SetEventOnCompletion(maxFv, evt);
            WaitForSingleObject(evt, INFINITE);
            CloseHandle(evt);
        }
    }

    entry = VideoDecoder_DX12{};
    m_decoderFreeList.push_back(decoder.handle_id);
    decoder.Reset();
}

// ===========================================================================
// CreateVideoOutputTexture — convenience wrapper that picks the right
// TextureDesc flags + format for a decoder output target.
// ===========================================================================

bool VideoDecoderDX12::CreateVideoOutputTexture(uint32_t width, uint32_t height,
                                                 RHI::VideoCodec compatibility,
                                                 RHI::Texture& outTexture)
{
    RHI::TextureDesc td{};
    td.type        = RHI::TextureDesc::Type::TEXTURE_2D;
    td.format      = RHI::Format::NV12;
    td.usage       = RHI::Usage::DEFAULT;
    td.width       = width;
    td.height      = height;
    td.array_size  = 1;
    td.mip_levels  = 1;
    td.bind_flags  = RHI::BindFlag::SHADER_RESOURCE;
    td.misc_flags  = RHI::ResourceMiscFlag::VIDEO_DECODE
                   | ((compatibility == RHI::VideoCodec::H264)
                      ? RHI::ResourceMiscFlag::VIDEO_COMPATIBILITY_H264
                      : RHI::ResourceMiscFlag::VIDEO_COMPATIBILITY_H265);
    td.layout      = RHI::ResourceState::UNDEFINED; // backend keeps it in COMMON
    td.debug_name  = "VideoDecodeOutput";

    return m_gfx.CreateTexture(td, outTexture, nullptr);
}

} // namespace RHI::DX12
