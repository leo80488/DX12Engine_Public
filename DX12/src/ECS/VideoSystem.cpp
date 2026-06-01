#include "ECS/VideoSystem.h"
#include "ECS/VideoComponent.h"
#include "ECS/ECS.h"
#include "ECS/FrameContext.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/IVideoDecoder.h"

#include "System/Log.h"

#include <algorithm>
#include <vector>

// Lazy: allocate the decoder + NV12 DPB textures the first time we see this
// VideoComponent. Returns true once everything is in place; false on any
// driver / HW failure (caller skips this entity until the user replaces the
// frameSource or restarts the engine).
//
// IMPORTANT: caller MUST have verified vc.frameSource != nullptr before
// calling. Without a bitstream source the D3D12 video decoder has nothing
// to feed and the DPB allocation would just leak.
static bool EnsureDecoderReady(IGraphicsDevice& gfx,
                                RHI::IVideoDecoderBackend& backend,
                                VideoComponent& vc)
{
    if (vc.decoder.IsValid()) return true;

    RHI::VideoDecoderDesc dd{};
    dd.codec        = vc.codec;
    dd.profile      = vc.profile;
    dd.maxWidth     = vc.width;
    dd.maxHeight    = vc.height;
    dd.bitDepth     = vc.bitDepth;
    dd.dpbSlotCount = (std::max)(vc.dpbSlotCount, 1u);
    dd.debugName    = "VideoComponentDecoder";

    if (!backend.CreateVideoDecoder(dd, vc.decoder))
    {
        LOG_ERROR("VideoSystem: CreateVideoDecoder failed — disabling entity");
        vc.state = VideoPlaybackState::Stopped;
        return false;
    }

    // Defer-release ANY pre-existing dpb textures (LoadTestPattern's NV12,
    // a stale Mp4FrameSource texture, etc.) before re-assigning. Without
    // this the old handles leak — they stay live in the GPU pool but the
    // VideoComponent no longer references them.
    for (auto& oldTex : vc.dpb)
        if (oldTex.IsValid()) gfx.DestroyTexture(oldTex);
    vc.dpb.assign(dd.dpbSlotCount, RHI::Texture{});

    // Allocate the NV12 DPB pool. Each slot is BOTH a decoder reference target
    // AND shader-readable (Y + UV plane SRVs), so caller's YUV→RGB PS can
    // sample any decoded frame the moment it retires.
    for (uint32_t i = 0; i < dd.dpbSlotCount; ++i)
    {
        if (!backend.CreateVideoOutputTexture(vc.width, vc.height, vc.codec, vc.dpb[i]))
        {
            LOG_ERROR("VideoSystem: CreateVideoOutputTexture slot %u failed", i);
            for (uint32_t j = 0; j < i; ++j)
                if (vc.dpb[j].IsValid()) gfx.DestroyTexture(vc.dpb[j]);
            vc.dpb.clear();
            backend.DestroyVideoDecoder(vc.decoder);
            vc.state = VideoPlaybackState::Stopped;
            return false;
        }
    }
    vc.currentDpbSlot = ~0u;
    vc.lastDecodedPts = -1.0;
    vc.framesDecoded  = 0;
    return true;
}

// Pull + decode as many ready frames as possible this tick (no more than 4
// to avoid stalling on a long catch-up if the entity was paused for a while).
static void PumpDecode(RHI::IVideoDecoderBackend& backend,
                       VideoComponent& vc)
{
    if (!vc.frameSource) return;

    constexpr int kMaxPumpThisTick = 4;
    for (int iter = 0; iter < kMaxPumpThisTick; ++iter)
    {
        Video::AccessUnit au;
        if (!vc.frameSource->Pull(au))
        {
            // End of stream.
            if (vc.loop)
            {
                vc.frameSource->Restart();
                vc.clockSeconds   = 0.0;
                vc.lastDecodedPts = -1.0;
                continue;
            }
            vc.state = VideoPlaybackState::Stopped;
            return;
        }

        // If the pulled frame is still in the future, hold it (the source must
        // be re-pullable on the next tick — IVideoFrameSource impls typically
        // buffer one frame ahead so a re-pull is cheap).
        // NOTE: production impls should expose a Peek()-style API so we don't
        // need to consume the frame to inspect its PTS. The current contract
        // assumes Pull is non-destructive until the engine decides to decode.
        if (au.ptsSeconds > vc.clockSeconds) return;

        // Resolve DPB slot indices to RHI::Texture pointers.
        constexpr uint32_t kMaxRefs = 17;
        const RHI::Texture* refs[kMaxRefs] = {};
        const uint32_t refCount = (std::min)(au.referenceSlotCount, kMaxRefs);
        for (uint32_t i = 0; i < refCount; ++i)
        {
            const uint32_t slot = au.referenceSlots[i];
            refs[i] = (slot < vc.dpb.size()) ? &vc.dpb[slot] : nullptr;
        }

        const uint32_t outSlot = (au.outputDpbSlot < vc.dpb.size())
                                ? au.outputDpbSlot : 0u;

        RHI::VideoDecodeArgs da{};
        da.bitstream          = au.bitstream;
        da.bitstreamBytes     = au.bitstreamBytes;
        da.sliceControl       = au.sliceControl;
        da.sliceCount         = au.sliceCount;
        da.pictureParams      = au.pictureParams;
        da.pictureParamsBytes = au.pictureParamsBytes;
        da.quantMatrices      = au.quantMatrices;
        da.quantMatricesBytes = au.quantMatricesBytes;
        da.outputTexture      = &vc.dpb[outSlot];
        da.dpbReferences      = refs;
        da.dpbReferenceCount  = refCount;
        da.frameIndex         = vc.framesDecoded;
        da.isReferenceFrame   = au.isReferenceFrame;

        if (!backend.DecodeVideoFrame(vc.decoder, da))
        {
            LOG_ERROR("VideoSystem: DecodeVideoFrame failed (pts=%.3f) — stopping playback",
                      au.ptsSeconds);
            vc.state = VideoPlaybackState::Stopped;
            return;
        }
        ++vc.framesDecoded;
        vc.currentDpbSlot = outSlot;
        vc.lastDecodedPts = au.ptsSeconds;
    }
}

void VideoSystem::Update(World& world, const FrameContext& ctx)
{
    RHI::IVideoDecoderBackend* backend = m_gfx.GetVideoBackend();
    if (!backend) return; // HW / driver does not support video decode.

    world.ForEach<VideoComponent>(
        [&](Entity /*e*/, VideoComponent& vc)
    {
        if (vc.state == VideoPlaybackState::Stopped) return;

        const double dt = static_cast<double>(ctx.deltaTime)
                        * static_cast<double>((std::max)(vc.playRate, 0.0f));

        // Path selection — first match wins:
        //   (1) decodedFrameSource (FFmpeg / Media Foundation; the GPU-
        //       accelerated mp4 playback path)
        //   (2) frameSource (engine's hand-rolled D3D12 video decoder)
        //   (3) no source — VideoSystem is a no-op (LoadTestPattern path).
        if (vc.decodedFrameSource)
        {
            if (vc.state == VideoPlaybackState::Playing)
            {
                vc.clockSeconds += dt;
                if (!vc.decodedFrameSource->Tick(m_gfx, vc, dt))
                {
                    LOG_ERROR("VideoSystem: decoded frame source failed — stopping");
                    vc.state = VideoPlaybackState::Stopped;
                }
            }
            return;
        }

        if (vc.frameSource)
        {
            if (!EnsureDecoderReady(m_gfx, *backend, vc)) return;
            if (vc.state == VideoPlaybackState::Playing)
            {
                vc.clockSeconds += dt;
                PumpDecode(*backend, vc);
            }
            return;
        }
        // No source — author-driven dpb (LoadTestPattern, manual upload).
        // VideoSystem leaves everything alone; renderer keeps sampling
        // whatever currentDpbSlot last pointed at.
    });
}
