#pragma once

// VideoSystem — drives every VideoComponent's playback clock + decode.
//
// Phase: GameplayPreLogic — runs before AI / animation / render. Decode work
// submits on the dedicated D3D12 video queue (see VideoDecoderDX12); the
// graphics queue stays free to do its normal frame. AddDecodeDependency on
// the render path inserts the cross-queue wait so sampling the NV12 output
// from a PS sees the decoded data.
//
// Per-entity per-tick flow:
//   1. If state != Playing: skip.
//   2. First tick for this entity: lazily create RHI::VideoDecoder + the
//      NV12 DPB texture array via IVideoDecoderBackend.
//   3. Advance clockSeconds by deltaTime * playRate.
//   4. While Pull() returns a frame whose PTS <= clockSeconds, submit it to
//      DecodeVideoFrame and update currentDpbSlot.
//   5. On Pull() == false (EOS): loop or stop based on VideoComponent::loop.
//
// Renderer integration: a thin Renderer-side helper (not part of this header)
// reads VideoComponent::currentDpbSlot + the matching dpb[] entry to bind
// the NV12 Y + UV plane SRVs into a YUV-RGB conversion PS.

#include "ECS/ISystem.h"

class IGraphicsDevice;

class VideoSystem : public SystemInPhase<TickPhase::GameplayPreLogic>
{
public:
    explicit VideoSystem(IGraphicsDevice& gfx) : m_gfx(gfx) {}

    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "VideoSystem"; }

    // The system mutates VideoComponent state (decoder handle, clock) and
    // submits GPU work — keep Opaque so it serialises against future
    // systems that may read VideoComponent from a worker thread.
    void DeclareAccess(SystemAccessBuilder& b) const override { b.OpaqueAccess(); }

private:
    IGraphicsDevice& m_gfx;
};
