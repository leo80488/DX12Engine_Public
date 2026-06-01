#pragma once

// VideoComponent — per-entity video playback state.
//
// The engine takes ownership of GPU resources (decoder, NV12 output texture,
// DPB ring) but stays codec-agnostic at the bitstream layer. The actual mp4/
// mkv demux + NAL parsing lives upstream — caller hands frames to the system
// through an `IVideoFrameSource` whose Pull() returns the next access unit.
//
// Components used at runtime:
//   * decoder       — RHI::VideoDecoder handle, created lazily on first tick
//   * outputTexture — NV12 RHI::Texture the system samples for rendering
//   * dpb[]         — pool of NV12 textures rotated as reference frames
//   * frameSource   — std::shared_ptr<IVideoFrameSource> (caller-owned)
//
// Lifecycle:
//   1. Author code: create entity, attach VideoComponent with codec/profile,
//      width/height, and frameSource pointing at the caller's demuxer.
//   2. VideoSystem::Update — first tick allocates decoder + NV12 textures,
//      subsequent ticks pull AccessUnit when (curPts >= nextDueTime).
//   3. After decode, currentOutputTexture is valid for renderer sampling
//      until the next pull replaces it.
//   4. Component destruction defer-releases the decoder + textures via the
//      backend's standard release path.

#include "Graphics/IVideoDecoder.h"

#include <cstdint>
#include <memory>
#include <vector>

class IGraphicsDevice;
struct VideoComponent;   // forward decl so Video::IDecodedFrameSource::Tick can take it by reference

namespace Video
{
    // One access unit (one coded picture) produced by the user-owned demuxer.
    //
    // Pointer lifetimes:
    //   The engine copies bitstream + picture-params into its own staging
    //   buffers inside the same DecodeVideoFrame call. Once Pull() returns,
    //   the caller MAY free / reuse the buffers immediately — no need to keep
    //   them alive across frames.
    //
    // refSlotIndices: the DPB slot indices (into VideoComponent::dpb[]) that
    // this picture references. The caller is responsible for tracking the
    // SPS/PPS reference list as defined by the codec — the engine just
    // forwards the handles.
    struct AccessUnit
    {
        const void* bitstream            = nullptr;
        size_t      bitstreamBytes       = 0;
        const void* pictureParams        = nullptr;
        size_t      pictureParamsBytes   = 0;
        const void* sliceControl         = nullptr;
        size_t      sliceCount           = 0;
        const void* quantMatrices        = nullptr;
        size_t      quantMatricesBytes   = 0;

        // Which existing DPB slot should hold the decoded output. Caller
        // manages DPB rotation; engine just maps slot → NV12 texture.
        uint32_t    outputDpbSlot        = 0;

        // Sub-array of slot indices the decoder will read from. May be empty
        // for I-frames. Engine resolves each slot index → DPB texture.
        const uint32_t* referenceSlots   = nullptr;
        uint32_t    referenceSlotCount   = 0;

        // Presentation timestamp (seconds) of this picture in the stream's
        // own time base. VideoSystem compares against an internal clock to
        // know when to display the frame.
        double      ptsSeconds           = 0.0;

        // True if this picture is itself a reference for future frames
        // (I/P; almost everything except non-reference B).
        bool        isReferenceFrame     = true;
    };

    // Abstract bitstream source. Engine pulls one AccessUnit per video tick
    // when it decides to advance. Returning false means "no frame available
    // this tick" — playback simply waits and Pull() will be retried next
    // frame.
    class IVideoFrameSource
    {
    public:
        virtual ~IVideoFrameSource() = default;

        // Populate @p outAU with the next access unit. Return false on EOS
        // (handled by VideoSystem: loop or stop based on VideoComponent).
        virtual bool Pull(AccessUnit& outAU) = 0;

        // Reset the underlying stream to the first frame. Called by
        // VideoSystem when loop=true and Pull() returned EOS, and by
        // Video::Seek(vc, 0).
        virtual void Restart() {}

        // Jump to the nearest keyframe at or before @p timeSeconds. The next
        // Pull() should return the frame whose decoded image will be the
        // requested presentation time once decoding catches up. Optional —
        // default is no-op (the system falls back to clock-only seeking).
        virtual void Seek(double /*timeSeconds*/) {}

        // Total stream duration in seconds (negative = unknown / live). Used
        // only for the editor inspector's progress bar.
        virtual double GetDurationSeconds() const { return -1.0; }
    };

    // Decoded-frame source — alternative to the bitstream IVideoFrameSource.
    // Implementations own the entire decode pipeline (FFmpeg / Media
    // Foundation) and upload finished NV12 bytes directly into the
    // component's `dpb[]` texture each tick. Preferred over IVideoFrameSource
    // because it works out of the box (Mp4FrameSource ships now); the engine's
    // hand-rolled D3D12 decoder behind IVideoFrameSource has no built-in source.
    class IDecodedFrameSource
    {
    public:
        virtual ~IDecodedFrameSource() = default;

        // Called by VideoSystem each frame while the component is Playing.
        // Implementations should:
        //   1. Read / decode as many access units as needed to catch up to
        //      `vc.clockSeconds` (advanced by VideoSystem before the call).
        //   2. Lazy-create `vc.dpb[]` (one NV12 RHI::Texture sized to the
        //      decoded frame) on first frame if empty.
        //   3. Upload the decoded NV12 bytes into `vc.dpb[0]` and set
        //      `vc.currentDpbSlot = 0`.
        // Return false on hard failure (file gone, decode reset failed) —
        // VideoSystem transitions the component to Stopped.
        virtual bool Tick(IGraphicsDevice& gfx, VideoComponent& vc,
                          double deltaSeconds) = 0;

        // Same semantics as IVideoFrameSource::Restart / Seek.
        virtual void   Restart() {}
        virtual void   Seek(double /*timeSeconds*/) {}
        virtual double GetDurationSeconds() const { return -1.0; }
    };
} // namespace Video

enum class VideoPlaybackState : uint8_t
{
    Stopped, // No work — clock frozen, no decode.
    Playing, // Active decode driven by deltaTime.
    Paused,  // Clock frozen; the decoder keeps the most recent output frame.
};

struct VideoComponent
{
    // ---- Stream config — set BEFORE the system first ticks this entity. ---
    RHI::VideoCodec   codec       = RHI::VideoCodec::H264;
    RHI::VideoProfile profile     = RHI::VideoProfile::H264_High;
    uint32_t          width       = 1920;
    uint32_t          height      = 1080;
    uint32_t          bitDepth    = 8;
    // DPB depth (number of NV12 output textures the engine keeps live for
    // the codec to reference). H.264 Level 4.1 / 1080p typically needs 4-5.
    uint32_t          dpbSlotCount = 8;

    // ---- Playback knobs ---------------------------------------------------
    VideoPlaybackState state      = VideoPlaybackState::Stopped;
    bool               loop       = false;
    // Playback rate multiplier (1.0 = realtime, 2.0 = double-speed).
    float              playRate   = 1.0f;

    // ---- Frame sources ----------------------------------------------------
    // VideoSystem picks ONE path per tick, in priority:
    //   (1) decodedFrameSource — caller already decoded NV12 frames (FFmpeg
    //       SW or D3D12VA hwaccel via Mp4FrameSource, Media Foundation, etc.).
    //       Tick() uploads NV12 bytes / GPU-copies the texture into dpb[].
    //       **This is the path most projects want.** Mp4FrameSource using
    //       FFmpeg's AV_HWDEVICE_TYPE_D3D12VA already runs the entire
    //       NAL→DXVA→D3D12 video decode on the GPU through libavcodec.
    //   (2) frameSource — raw bitstream + DXVA picture-params for the
    //       engine's own ID3D12VideoDecoder. No built-in implementation
    //       ships with the engine; only useful if you write your own NAL +
    //       SPS/PPS parser.
    //   (3) Both null — VideoSystem leaves dpb[] alone. Suitable for
    //       LoadTestPattern (which seeds dpb[0] once and the renderer keeps
    //       sampling it).
    std::shared_ptr<Video::IDecodedFrameSource> decodedFrameSource;
    std::shared_ptr<Video::IVideoFrameSource>   frameSource;

    // ---- Surface mode ----------------------------------------------------
    // false (default) — VideoPass composites into the HDR target in screen
    //                   space using `rectUMin/Max, rectVMin/Max` below.
    // true            — VideoQuadPass draws a `worldWidth × worldHeight`
    //                   quad in world space at the entity's GlobalTransform
    //                   (identity if absent). Quad faces +Z in local space,
    //                   centred on the origin. Uses depth test so scene
    //                   geometry can occlude it like any other surface.
    bool             worldSpace  = false;
    float            worldWidth  = 1.0f;
    float            worldHeight = 1.0f;

    // ---- Screen-space surface params (VideoPass; ignored when worldSpace).
    // rectUV is the sub-rectangle of the HDR target the video occupies,
    // [0,0]..[1,1] = top-left to bottom-right of the screen. Default is
    // fullscreen.
    float            rectUMin   = 0.0f;
    float            rectVMin   = 0.0f;
    float            rectUMax   = 1.0f;
    float            rectVMax   = 1.0f;

    // ---- Common surface params (both passes consume) ---------------------
    // alpha multiplies the composited RGB (1 = opaque, <1 lets the
    // underlying scene bleed through). colorSpace selects the YUV→RGB
    // matrix: 0 = BT.709 (HD, default), 1 = BT.601 (SD).
    float            renderAlpha = 1.0f;
    uint32_t         colorSpace  = 0;

    // ---- Runtime state (managed by VideoSystem; don't write from author code)
    RHI::VideoDecoder        decoder;       // backend handle (~0u until first tick)
    std::vector<RHI::Texture> dpb;          // NV12 textures, size == dpbSlotCount
    // Index into dpb[] of the most recently decoded frame — what renderers
    // should sample this tick. Invalid (~0u) until first successful decode.
    uint32_t                  currentDpbSlot = ~0u;
    // Stream clock (seconds) driven by VideoSystem; used to decide when to
    // pull the next AccessUnit.
    double                    clockSeconds   = 0.0;
    // Last successfully decoded PTS — clock advances until it reaches the
    // NEXT pulled AU's pts, then decode + display.
    double                    lastDecodedPts = -1.0;
    // Monotonic frame counter for the per-decoder fence bookkeeping.
    uint64_t                  framesDecoded  = 0;
};
