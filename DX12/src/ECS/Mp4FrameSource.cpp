#include "ECS/Mp4FrameSource.h"
#include "ECS/VideoComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

// ===========================================================================
// FFmpeg-backed implementation (compiled only when WITH_FFMPEG is defined).
// Stub implementation lower down handles the no-FFmpeg case so the rest of
// the engine builds without the dependency.
// ===========================================================================

#ifdef WITH_FFMPEG

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")

#include <vector>

namespace Video {

class Mp4FrameSource final : public IDecodedFrameSource
{
public:
    bool Open(IGraphicsDevice& gfx, const std::string& path);
    ~Mp4FrameSource() override { Close(); }

    bool   Tick(IGraphicsDevice& gfx, VideoComponent& vc, double dtSeconds) override;
    void   Restart() override;
    void   Seek(double timeSeconds) override;
    double GetDurationSeconds() const override { return m_durationSec; }

private:
    void   Close();
    bool   EnsureNV12Texture(IGraphicsDevice& gfx, VideoComponent& vc);
    bool   UploadHwFrame(IGraphicsDevice& gfx, VideoComponent& vc);   // D3D12VA path
    bool   UploadSwFrame(IGraphicsDevice& gfx, VideoComponent& vc);   // software path

    static enum AVPixelFormat GetHwFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);

    AVFormatContext* m_fmt        = nullptr;
    AVCodecContext*  m_codec      = nullptr;
    AVFrame*         m_frame      = nullptr;
    AVFrame*         m_nv12Frame  = nullptr;   // sws output staging (sw path only)
    AVPacket*        m_packet     = nullptr;
    SwsContext*      m_sws        = nullptr;
    AVBufferRef*     m_hwDeviceCtx = nullptr;  // D3D12VA hwdevice (null when sw)
    bool             m_hwEnabled  = false;     // D3D12VA path actually being used
    int              m_videoIdx   = -1;
    AVRational       m_streamTb   { 0, 1 };    // time base
    int              m_width      = 0;
    int              m_height     = 0;
    double           m_durationSec = -1.0;
    bool             m_eos        = false;
    double           m_lastPts    = -1.0;
    bool             m_textureCreated = false;
};

// Codec get_format callback: prefer AV_PIX_FMT_D3D12 if the codec offers it,
// else return the first sw fallback. Called by libavcodec during avcodec_open2.
// Returning a non-hw format from this falls back to software decode for the
// rest of the stream — gives us a transparent fallback on driver issues.
enum AVPixelFormat Mp4FrameSource::GetHwFormat(AVCodecContext* /*ctx*/,
                                                const AVPixelFormat* pix_fmts)
{
    for (const AVPixelFormat* p = pix_fmts; p && *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == AV_PIX_FMT_D3D12) return AV_PIX_FMT_D3D12;
    }
    return (pix_fmts && pix_fmts[0] != AV_PIX_FMT_NONE) ? pix_fmts[0]
                                                        : AV_PIX_FMT_NONE;
}

// ---------------------------------------------------------------------------
bool Mp4FrameSource::Open(IGraphicsDevice& gfx, const std::string& path)
{
    if (avformat_open_input(&m_fmt, path.c_str(), nullptr, nullptr) < 0)
    {
        LOG_ERROR("Mp4FrameSource: avformat_open_input failed on '%s'", path.c_str());
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0)
    {
        LOG_ERROR("Mp4FrameSource: avformat_find_stream_info failed");
        Close();
        return false;
    }

    const AVCodec* dec = nullptr;
    m_videoIdx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (m_videoIdx < 0 || !dec)
    {
        LOG_ERROR("Mp4FrameSource: no video stream / decoder for '%s'", path.c_str());
        Close();
        return false;
    }

    AVStream* stream = m_fmt->streams[m_videoIdx];
    m_streamTb = stream->time_base;
    m_durationSec = (stream->duration > 0 && stream->time_base.den > 0)
        ? double(stream->duration) * av_q2d(stream->time_base)
        : (m_fmt->duration > 0 ? double(m_fmt->duration) / AV_TIME_BASE : -1.0);

    m_codec = avcodec_alloc_context3(dec);
    if (!m_codec || avcodec_parameters_to_context(m_codec, stream->codecpar) < 0)
    {
        LOG_ERROR("Mp4FrameSource: codec context init failed");
        Close();
        return false;
    }

    // ----- Try D3D12VA hwaccel using OUR existing ID3D12Device ------------
    // Allocate the hwdevice context manually (instead of
    // av_hwdevice_ctx_create) so we can hand over our pre-existing
    // ID3D12Device + ID3D12VideoDevice rather than letting FFmpeg create
    // a second one. Failure anywhere here → fall through to software decode.
    auto* dx12 = dynamic_cast<GraphicsDX12*>(&gfx);
    if (dx12 && dx12->GetDevice())
    {
        m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
        if (m_hwDeviceCtx)
        {
            auto* hwctx = (AVHWDeviceContext*)m_hwDeviceCtx->data;
            auto* d3d12 = (AVD3D12VADeviceContext*)hwctx->hwctx;

            // av_hwdevice_ctx_init expects ownership of the device — it
            // calls Release on shutdown. AddRef ours so the engine's
            // ID3D12Device survives FFmpeg's Release.
            d3d12->device = dx12->GetDevice();
            d3d12->device->AddRef();

            if (av_hwdevice_ctx_init(m_hwDeviceCtx) == 0)
            {
                m_codec->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
                m_codec->get_format    = &Mp4FrameSource::GetHwFormat;
                m_hwEnabled = true;
                LOG_INFO("Mp4FrameSource: D3D12VA hwaccel enabled (codec=%s)", dec->name);
            }
            else
            {
                LOG_WARNING("Mp4FrameSource: av_hwdevice_ctx_init failed — "
                            "falling back to software decode");
                av_buffer_unref(&m_hwDeviceCtx);
                m_hwDeviceCtx = nullptr;
            }
        }
        else
        {
            LOG_WARNING("Mp4FrameSource: av_hwdevice_ctx_alloc(D3D12VA) failed");
        }
    }

    if (avcodec_open2(m_codec, dec, nullptr) < 0)
    {
        LOG_ERROR("Mp4FrameSource: avcodec_open2 failed");
        Close();
        return false;
    }

    m_width  = m_codec->width;
    m_height = m_codec->height;
    if ((m_width & 1) || (m_height & 1))
    {
        LOG_ERROR("Mp4FrameSource: odd dimensions %dx%d (NV12 needs even)",
                  m_width, m_height);
        Close();
        return false;
    }

    m_frame      = av_frame_alloc();
    m_nv12Frame  = av_frame_alloc();
    m_packet     = av_packet_alloc();
    if (!m_frame || !m_nv12Frame || !m_packet) { Close(); return false; }

    // Pre-allocate NV12 output frame.
    m_nv12Frame->format = AV_PIX_FMT_NV12;
    m_nv12Frame->width  = m_width;
    m_nv12Frame->height = m_height;
    if (av_frame_get_buffer(m_nv12Frame, 32) < 0)
    {
        LOG_ERROR("Mp4FrameSource: NV12 frame buffer alloc failed");
        Close();
        return false;
    }

    LOG_SUCCESS("Mp4FrameSource: opened '%s' %dx%d codec=%s duration=%.3fs",
                path.c_str(), m_width, m_height, dec->name, m_durationSec);
    return true;
}

void Mp4FrameSource::Close()
{
    if (m_sws)         { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_packet)      { av_packet_free(&m_packet); }
    if (m_nv12Frame)   { av_frame_free(&m_nv12Frame); }
    if (m_frame)       { av_frame_free(&m_frame); }
    if (m_codec)       { avcodec_free_context(&m_codec); }
    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); }
    if (m_fmt)         { avformat_close_input(&m_fmt); }
    m_hwEnabled = false;
    m_textureCreated = false;
    m_eos = false;
    m_lastPts = -1.0;
}

void Mp4FrameSource::Restart()
{
    if (!m_fmt) return;
    av_seek_frame(m_fmt, m_videoIdx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    m_eos = false;
    m_lastPts = -1.0;
}

void Mp4FrameSource::Seek(double timeSeconds)
{
    if (!m_fmt) return;
    if (timeSeconds < 0.0) timeSeconds = 0.0;
    const int64_t ts = static_cast<int64_t>(timeSeconds / av_q2d(m_streamTb));
    av_seek_frame(m_fmt, m_videoIdx, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    m_eos = false;
    m_lastPts = -1.0;
}

bool Mp4FrameSource::EnsureNV12Texture(IGraphicsDevice& gfx, VideoComponent& vc)
{
    if (m_textureCreated && !vc.dpb.empty() && vc.dpb[0].IsValid())
        return true;

    // Free anything that was there before (test pattern, prior open, etc.).
    for (auto& tex : vc.dpb) if (tex.IsValid()) gfx.DestroyTexture(tex);
    vc.dpb.clear();
    vc.dpb.resize(1);

    RHI::TextureDesc td{};
    td.type        = RHI::TextureDesc::Type::TEXTURE_2D;
    td.format      = RHI::Format::NV12;
    td.usage       = RHI::Usage::DEFAULT;
    td.width       = static_cast<uint32_t>(m_width);
    td.height      = static_cast<uint32_t>(m_height);
    td.array_size  = 1;
    td.mip_levels  = 1;
    td.bind_flags  = RHI::BindFlag::SHADER_RESOURCE;
    td.misc_flags  = RHI::ResourceMiscFlag::VIDEO_DECODE
                   | RHI::ResourceMiscFlag::VIDEO_COMPATIBILITY_H264;
    td.layout      = RHI::ResourceState::SHADER_RESOURCE;
    td.debug_name  = "Mp4FrameSource.NV12";

    if (!gfx.CreateTexture(td, vc.dpb[0], nullptr))
    {
        LOG_ERROR("Mp4FrameSource: NV12 dpb texture create failed");
        vc.dpb.clear();
        return false;
    }
    vc.currentDpbSlot = 0;
    vc.width  = static_cast<uint32_t>(m_width);
    vc.height = static_cast<uint32_t>(m_height);
    m_textureCreated = true;
    return true;
}

// D3D12VA hwaccel path — the decoded frame IS already an NV12 ID3D12Resource
// living on our engine's device. Just GPU-copy it into vc.dpb[0].
bool Mp4FrameSource::UploadHwFrame(IGraphicsDevice& gfx, VideoComponent& vc)
{
    auto* d3d12Frame = reinterpret_cast<AVD3D12VAFrame*>(m_frame->data[0]);
    if (!d3d12Frame || !d3d12Frame->texture)
    {
        LOG_ERROR("Mp4FrameSource: hwaccel frame has no texture");
        return false;
    }
    auto* dx12 = dynamic_cast<GraphicsDX12*>(&gfx);
    if (!dx12)
    {
        LOG_ERROR("Mp4FrameSource: hwaccel requires GraphicsDX12 backend");
        return false;
    }
    return dx12->CopyD3D12ResourceToTexture(
        vc.dpb[0],
        d3d12Frame->texture,
        d3d12Frame->sync_ctx.fence,
        d3d12Frame->sync_ctx.fence_value);
}

// Software decode fallback — sws_scale into an UPLOAD-style NV12 staging
// frame, then UpdateTexture-style CPU upload.
bool Mp4FrameSource::UploadSwFrame(IGraphicsDevice& gfx, VideoComponent& vc)
{
    if (!m_sws || m_codec->pix_fmt != m_frame->format)
    {
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
        m_sws = sws_getContext(m_width, m_height, (AVPixelFormat)m_frame->format,
                               m_width, m_height, AV_PIX_FMT_NV12,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws)
        {
            LOG_ERROR("Mp4FrameSource: sws_getContext failed (pix_fmt=%d)",
                      m_frame->format);
            return false;
        }
    }
    sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_height,
              m_nv12Frame->data, m_nv12Frame->linesize);

    RHI::SubresourceData planes[2]{};
    planes[0].data_ptr    = m_nv12Frame->data[0];
    planes[0].row_pitch   = static_cast<uint32_t>(m_nv12Frame->linesize[0]);
    planes[0].slice_pitch = planes[0].row_pitch * static_cast<uint32_t>(m_height);
    planes[1].data_ptr    = m_nv12Frame->data[1];
    planes[1].row_pitch   = static_cast<uint32_t>(m_nv12Frame->linesize[1]);
    planes[1].slice_pitch = planes[1].row_pitch * static_cast<uint32_t>(m_height / 2);

    return gfx.UpdateTexture(vc.dpb[0], planes, 2);
}

bool Mp4FrameSource::Tick(IGraphicsDevice& gfx, VideoComponent& vc, double /*dt*/)
{
    if (!m_fmt) return false;

    // EOS handling: honour VideoComponent::loop the same way VideoSystem's
    // bitstream path does — auto-restart when loop is on, else hold the last
    // frame and let the user explicitly Restart() / Stop().
    if (m_eos)
    {
        if (!vc.loop) return true;
        Restart();
        vc.clockSeconds   = 0.0;
        vc.lastDecodedPts = -1.0;
    }

    if (!EnsureNV12Texture(gfx, vc)) return false;

    // Decode-ahead loop: read packets until decoded frame PTS catches up
    // with `vc.clockSeconds`. Stops as soon as we have a frame at or past
    // the clock so subsequent ticks can advance one frame at a time.
    while (true)
    {
        const double target = vc.clockSeconds;
        if (m_lastPts >= 0.0 && m_lastPts >= target) break;

        int ret = av_read_frame(m_fmt, m_packet);
        if (ret == AVERROR_EOF)
        {
            // Flush decoder; on EOF send NULL packet then drain frames.
            avcodec_send_packet(m_codec, nullptr);
            ret = avcodec_receive_frame(m_codec, m_frame);
            if (ret == 0)
            {
                m_lastPts = m_frame->pts * av_q2d(m_streamTb);
                vc.lastDecodedPts = m_lastPts;
                ++vc.framesDecoded;
                if (m_frame->format == AV_PIX_FMT_D3D12) UploadHwFrame(gfx, vc);
                else                                     UploadSwFrame(gfx, vc);
                continue;
            }
            // True end-of-stream: tell VideoSystem to loop or stop based on
            // VideoComponent::loop. Returning true keeps the source alive; the
            // upper layer calls Restart() when loop=true.
            m_eos = true;
            return true;
        }
        if (ret < 0)
        {
            LOG_ERROR("Mp4FrameSource: av_read_frame error %d", ret);
            return false;
        }

        if (m_packet->stream_index != m_videoIdx)
        {
            av_packet_unref(m_packet);
            continue;
        }

        if (avcodec_send_packet(m_codec, m_packet) < 0)
        {
            av_packet_unref(m_packet);
            continue;
        }
        av_packet_unref(m_packet);

        while (avcodec_receive_frame(m_codec, m_frame) == 0)
        {
            m_lastPts = m_frame->pts * av_q2d(m_streamTb);
            vc.lastDecodedPts = m_lastPts;
            ++vc.framesDecoded;
            if (m_frame->format == AV_PIX_FMT_D3D12) UploadHwFrame(gfx, vc);
            else                                     UploadSwFrame(gfx, vc);
            if (m_lastPts >= target) break;
        }
    }
    return true;
}

std::shared_ptr<IDecodedFrameSource> OpenMp4(IGraphicsDevice& gfx, const std::string& path)
{
    auto src = std::make_shared<Mp4FrameSource>();
    if (!src->Open(gfx, path)) return nullptr;
    return src;
}

} // namespace Video

#else  // WITH_FFMPEG not defined

namespace Video {
std::shared_ptr<IDecodedFrameSource> OpenMp4(IGraphicsDevice& /*gfx*/, const std::string& path)
{
    LOG_WARNING("Video::OpenMp4('%s') called but WITH_FFMPEG is not defined. "
                "See include/ECS/Mp4FrameSource.h for the vcpkg install steps.",
                path.c_str());
    return nullptr;
}
} // namespace Video

#endif // WITH_FFMPEG
