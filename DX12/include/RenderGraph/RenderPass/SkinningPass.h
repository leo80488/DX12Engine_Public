#pragma once

// SkinningPass — compute pass that skins all visible skinned entities.
//
// Executes BEFORE GBufferPass. For each skinned entity:
//   1. Binds rest-pose buffers (pos, nrm) and blend buffer as SRVs.
//   2. Binds the per-frame PoseRingBuffer as SRV (bone matrices).
//   3. Binds SkinnedVertexRing pos/nrm buffers as UAVs.
//   4. Dispatches ceil(vertexCount / 64) thread groups.
//
// After all dispatches, issues a UAV barrier so GBufferPass can safely read
// the SkinnedVertexRing buffers as SRVs via MeshDescriptor StreamDescriptors.
//
// No RenderGraph texture declarations needed (operates on GPU buffers only).
//
// The Renderer supplies per-entity job descriptors via SetJobs() before Execute().

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/SkinningBuffers.h"

#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Per-entity dispatch descriptor (built by Renderer::BeginFrame, consumed by Execute)
// ---------------------------------------------------------------------------
struct SkinDispatchDesc
{
    uint64_t restPosSRVHandle;       // GPU handle for rest position buffer SRV
    uint64_t restNrmSRVHandle;       // GPU handle for rest normal buffer SRV
    uint64_t blendSRVHandle;         // GPU handle for blend-weight buffer SRV
    uint64_t morphDeltaSRVHandle;    // GPU handle for morph target delta buffer SRV (0 = no morphs)
    uint32_t poseByteOffset;         // byte offset into current frame's PoseRingBuffer
    uint32_t outPosByteOffset;       // byte offset into SkinnedVertexRing pos buffer
    uint32_t outNrmByteOffset;       // byte offset into SkinnedVertexRing nrm buffer
    uint32_t vertexCount;
    uint32_t morphCount;             // number of morph targets (0 = no morphs)

    // TAA velocity: previous frame bone matrices for per-vertex motion vectors.
    uint32_t prevPoseByteOffset;     // byte offset into prev frame PoseRingBuffer (0xFFFFFFFF = none)
    uint32_t outPrevPosByteOffset;   // byte offset into SkinnedVertexRing prev-pos buffer

    float    morphWeights[128];      // per-target weights from MorphComponent
};

// ---------------------------------------------------------------------------
// SkinningPass
// ---------------------------------------------------------------------------
class SkinningPass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "SkinningPass"; }

    void Setup  (RG::RenderGraphBuilder& b) override {}  // no graph-managed textures
    void Init   (IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // Set per-frame ring buffer references (non-owning; owned by Renderer).
    void SetBuffers(PoseRingBuffer* pose, SkinnedVertexRing* verts)
    {
        m_poseBuffer = pose;
        m_vertRing   = verts;
    }

    // Supply per-entity jobs built by Renderer::BeginFrame.
    // Pointer must remain valid until Execute() returns.
    void SetJobs(const std::vector<SkinDispatchDesc>* jobs) { m_jobs = jobs; }

private:
    // Compute root sig slots (must match GraphicsDX12::CreateComputeRootSignature)
    static constexpr uint32_t kCBSlot    = 0;  // ROOT_CBV b0 space2 (unused now, kept for compat)
    static constexpr uint32_t kRestPos   = 1;  // DESC_TABLE t0 space2
    static constexpr uint32_t kRestNrm   = 2;  // DESC_TABLE t1 space2
    static constexpr uint32_t kBlend     = 3;  // DESC_TABLE t2 space2
    static constexpr uint32_t kPose        = 7;  // DESC_TABLE t3 space2
    static constexpr uint32_t kMorphDeltas  = 8;  // DESC_TABLE t4 space2
    static constexpr uint32_t kMorphWeights = 6;  // DESC_TABLE t5 space2
    // (job data is passed via root CBV pointing at each job's 256B slot)
    static constexpr uint32_t kOutPos       = 4;  // DESC_TABLE u0 space2
    static constexpr uint32_t kOutNrm       = 5;  // DESC_TABLE u1 space2

    // Max skinned meshes per frame. StructuredBuffer has no size limit.
    static constexpr uint32_t kMaxSkinJobs = 2048;

    IGraphicsDevice*   m_gfx     = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    // Job upload buffer: kMaxSkinJobs × 256 bytes (one 256B-aligned slot per job).
    // Root CBV points at each slot for per-dispatch binding.
    RHI::GPUBuffer m_jobBuffer;
    void*          m_jobBufferMapped = nullptr;

    // Morph weight upload buffer: kMaxSkinJobs × 512 bytes (128 floats per slot).
    RHI::GPUBuffer m_morphWeightBuf;
    void*          m_morphWeightMapped = nullptr;
    uint64_t       m_morphWeightSRV    = 0;

    PoseRingBuffer*   m_poseBuffer = nullptr;
    SkinnedVertexRing* m_vertRing  = nullptr;

    const std::vector<SkinDispatchDesc>* m_jobs = nullptr;
};
