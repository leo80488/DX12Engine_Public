#pragma once

// AfterimageCapturePass — compute pass that copies a slice of the post-skinning
// vertex output (SkinnedVertexRing pos/nrm SRVs) into long-lived snapshot
// slots in AfterimageSystem's pool. Runs RIGHT AFTER SkinningPass on the same
// command list, before any graph pass that reads SkinnedVertexRing as SRV.
//
// Why CS, not CopyBufferRegion: SkinnedVertexRing is one buffer with all
// skinned entities packed in; transitioning it to COPY_SOURCE would block
// the whole resource and force a full barrier flush. A CS reads the source
// via SRV (which it already exposes) and writes to AfterimagePool UAV — no
// state transitions on the source.
//
// Resource state contract:
//   - SkinnedVertexRing pos/nrm UAVs already had a UAV memory barrier emitted
//     by SkinningPass; we read them as SRVs in the same way GBufferPass does.
//   - AfterimagePool pos/nrm UAVs stay UAV across frames. After this pass we
//     emit a UAV memory barrier so GBuffer/Transparent can read them next.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <cstdint>

class AfterimageSystem;

class AfterimageCapturePass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "AfterimageCapturePass"; }

    void Setup(RG::RenderGraphBuilder& /*b*/) override {} // no graph textures
    void Init (IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    void SetSystem(AfterimageSystem* sys) { m_system = sys; }

private:
    // Compute root sig slots — same convention as SkinningPass so we reuse
    // the engine's compute root signature.
    static constexpr uint32_t kCBSlot  = 0;  // ROOT_CBV b0 space2 — AfterimageCopyJob
    static constexpr uint32_t kSrcPos  = 1;  // DESC_TABLE t0 space2 — SkinnedVertexRing pos SRV
    static constexpr uint32_t kSrcNrm  = 2;  // DESC_TABLE t1 space2 — SkinnedVertexRing nrm SRV
    static constexpr uint32_t kDstPos  = 4;  // DESC_TABLE u0 space2 — Afterimage pool pos UAV
    static constexpr uint32_t kDstNrm  = 5;  // DESC_TABLE u1 space2 — Afterimage pool nrm UAV

    // Same per-job CBV ring slot convention as SkinningPass (256-byte aligned).
    static constexpr uint32_t kMaxJobsPerFrame = 64;
    static constexpr uint32_t kJobSlotBytes    = 256;

    IGraphicsDevice*   m_gfx     = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    // Multi-slot per-job CBV pool packed into a single buffer, byte-offset
    // dispatched. Wrapped in FrameCB<JobBufferPool> so the buffer rings across
    // the 3-frame CPU pipeline depth — without this, two consecutive
    // SpawnAfterimage calls in successive frames write the same physical bytes
    // while the prior frame's dispatch is still consuming them.
    struct alignas(256) JobBufferPool
    {
        uint8_t bytes[kMaxJobsPerFrame * kJobSlotBytes];
    };
    FrameCB<JobBufferPool> m_jobCB;

    AfterimageSystem* m_system = nullptr;
};
