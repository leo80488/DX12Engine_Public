#include "ECS/VideoHelpers.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/IVideoDecoder.h"
#include "System/Log.h"

#include <vector>
#include <cstdint>

namespace Video
{

void Seek(VideoComponent& vc, double timeSeconds)
{
    if (timeSeconds < 0.0) timeSeconds = 0.0;
    vc.clockSeconds   = timeSeconds;
    vc.lastDecodedPts = -1.0;            // force next pump to deliver a frame

    if (vc.frameSource)         vc.frameSource->Seek(timeSeconds);
    if (vc.decodedFrameSource)  vc.decodedFrameSource->Seek(timeSeconds);
}

void ReleaseGPUResources(IGraphicsDevice& gfx, VideoComponent& vc)
{
    if (vc.decoder.IsValid())
    {
        if (auto* backend = gfx.GetVideoBackend())
            backend->DestroyVideoDecoder(vc.decoder);
        // If the backend went away the handle is orphaned — clear it so a
        // later ReleaseGPUResources doesn't re-enter the same path.
        vc.decoder.Reset();
    }
    for (auto& tex : vc.dpb)
    {
        if (tex.IsValid()) gfx.DestroyTexture(tex);
    }
    vc.dpb.clear();
    vc.currentDpbSlot = ~0u;
    vc.lastDecodedPts = -1.0;
    vc.framesDecoded  = 0;
}

// ---------------------------------------------------------------------------
// SMPTE 75 % colour bars (BT.709 limited-range NV12 byte values).
//
// Computed by converting the 8 RGB bars at 75 % intensity through the same
// matrix the VideoComposite PS undoes:
//   white   (191,191,191), yellow  (191,191,  0), cyan   (  0,191,191),
//   green   (  0,191,  0), magenta (191,  0,191), red    (191,  0,  0),
//   blue    (  0,  0,191), black   (  0,  0,  0)
//
// Pre-computed Y / U / V bytes per bar (limited range, [16..235] for Y;
// [16..240] for chroma centred on 128). Keeping them as a static table
// avoids burning floats at runtime.
// ---------------------------------------------------------------------------
static constexpr uint8_t kBarY [8] = { 180, 162, 131, 112,  84,  65,  35,  16 };
static constexpr uint8_t kBarCb[8] = { 128,  44, 156,  72, 184, 100, 212, 128 };
static constexpr uint8_t kBarCr[8] = { 128, 142,  44,  58, 198, 212, 114, 128 };

bool LoadTestPattern(IGraphicsDevice& gfx, VideoComponent& vc,
                     uint32_t width, uint32_t height)
{
    if ((width & 1u) || (height & 1u) || width == 0 || height == 0)
    {
        LOG_ERROR("Video::LoadTestPattern: width/height must be even and >0 (got %ux%u)",
                  width, height);
        return false;
    }

    // Re-init: drop any prior decoder + DPB textures, then mark the component
    // as externally-driven so VideoSystem doesn't touch GPU state on it.
    ReleaseGPUResources(gfx, vc);
    // Drop any frame sources so VideoSystem leaves dpb[] alone.
    vc.decodedFrameSource.reset();
    vc.frameSource.reset();
    vc.state         = VideoPlaybackState::Paused;
    vc.width         = width;
    vc.height        = height;

    // ---- Build Y / UV byte planes -----------------------------------------
    const size_t yPitch  = width;
    const size_t uvPitch = width;   // R8G8 = 2 bytes per chroma sample, half-res → width / 2 samples → width bytes
    std::vector<uint8_t> yPlane (yPitch  * height);
    std::vector<uint8_t> uvPlane(uvPitch * (height / 2));

    // Y plane — 8 vertical colour bars.
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t* row = yPlane.data() + y * yPitch;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t bar = (x * 8) / width;
            row[x] = kBarY[bar];
        }
    }
    // UV plane — half-res in both dimensions; one (Cb, Cr) pair per 2×2
    // luma block. Interleaved storage: byte 0 = Cb, byte 1 = Cr per sample.
    const uint32_t halfW = width  / 2;
    const uint32_t halfH = height / 2;
    for (uint32_t y = 0; y < halfH; ++y)
    {
        uint8_t* row = uvPlane.data() + y * uvPitch;
        for (uint32_t x = 0; x < halfW; ++x)
        {
            const uint32_t bar = ((x * 2) * 8) / width;
            row[2*x + 0] = kBarCb[bar];
            row[2*x + 1] = kBarCr[bar];
        }
    }

    // ---- Create the NV12 texture with both planes seeded ------------------
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
                   | RHI::ResourceMiscFlag::VIDEO_COMPATIBILITY_H264;
    td.layout      = RHI::ResourceState::SHADER_RESOURCE;
    td.debug_name  = "VideoTestPattern";

    // NV12 has 2 subresources: plane 0 = Y, plane 1 = UV (interleaved CbCr).
    RHI::SubresourceData planes[2]{};
    planes[0].data_ptr    = yPlane.data();
    planes[0].row_pitch   = static_cast<uint32_t>(yPitch);
    planes[0].slice_pitch = static_cast<uint32_t>(yPitch * height);
    planes[1].data_ptr    = uvPlane.data();
    planes[1].row_pitch   = static_cast<uint32_t>(uvPitch);
    planes[1].slice_pitch = static_cast<uint32_t>(uvPitch * halfH);

    vc.dpb.resize(1);
    if (!gfx.CreateTexture(td, vc.dpb[0], planes))
    {
        LOG_ERROR("Video::LoadTestPattern: CreateTexture(NV12) failed");
        vc.dpb.clear();
        return false;
    }
    vc.currentDpbSlot = 0;
    vc.framesDecoded  = 1;
    LOG_SUCCESS("Video::LoadTestPattern: %ux%u colour bars uploaded", width, height);
    return true;
}

} // namespace Video
