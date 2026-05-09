#include "RenderGraph/RenderPass/SkinningPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>

// GPU-side constant buffer layout — must match Skin.cs.hlsl SkinJobDesc
// Morph weights are stored in a separate ByteAddressBuffer, not in the CB.
struct SkinJobCB
{
    uint32_t poseByteOffset;
    uint32_t vertexCount;
    uint32_t outPosByteOffset;
    uint32_t outNrmByteOffset;
    uint32_t morphCount;             // 0 = no morphs
    uint32_t morphWeightByteOffset;  // byte offset into morph weight buffer
    uint32_t prevPoseByteOffset;     // 0xFFFFFFFF = no prev data
    uint32_t outPrevPosByteOffset;   // byte offset into prev pos output
};

// ---------------------------------------------------------------------------
void SkinningPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SkinCS, RHI::ShaderStage::CS, "Skin.cs.hlsl", "SkinCS");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SkinCS);
    if (!cs)
    {
        LOG_ERROR("SkinningPass: SkinCS shader not found");
        return;
    }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    {
        LOG_ERROR("SkinningPass: compute PSO creation failed");
        return;
    }

    // Job buffer: one 256-byte-aligned slot per job. Root CBV points at each
    // slot's offset. UPLOAD heap, no bind flags needed for root CBV.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxSkinJobs) * 256u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::NONE;
        if (gfx.CreateBuffer(bd, m_jobBuffer))
            m_jobBufferMapped = gfx.MapBuffer(m_jobBuffer);
        if (!m_jobBufferMapped)
            LOG_ERROR("SkinningPass: failed to map job buffer");
    }

    // Morph weight upload buffer: kMaxSkinJobs × 512 bytes.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxSkinJobs) * 512u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (gfx.CreateBuffer(bd, m_morphWeightBuf))
        {
            m_morphWeightMapped = gfx.MapBuffer(m_morphWeightBuf);
            m_morphWeightSRV    = gfx.GetBufferSRVGpuHandle(m_morphWeightBuf);
        }
        if (!m_morphWeightMapped)
            LOG_WARNING("SkinningPass: failed to map morph weight buffer (morphs disabled)");
    }

    LOG_SUCCESS("SkinningPass: initialised");
}

// ---------------------------------------------------------------------------
RHI::CommandList SkinningPass::Execute(RHI::CommandList cl)
{
    if (!m_pso.IsValid() || !m_jobBufferMapped) return cl;
    if (!m_jobs || m_jobs->empty())              return cl;
    if (!m_poseBuffer || !m_vertRing)            return cl;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // ---- Phase 1 (CPU): upload all jobs (256-byte aligned) + morph weights --
    uint32_t totalJobs = 0;

    for (const SkinDispatchDesc& job : *m_jobs)
    {
        if (job.vertexCount == 0) continue;
        if (totalJobs >= kMaxSkinJobs)
        {
            LOG_WARNING("SkinningPass: exceeded kMaxSkinJobs (%u) — remaining jobs skipped", kMaxSkinJobs);
            break;
        }

        // Write SkinJobCB at 256-byte aligned offset.
        SkinJobCB* slot = reinterpret_cast<SkinJobCB*>(
            static_cast<uint8_t*>(m_jobBufferMapped) + totalJobs * 256u);
        std::memset(slot, 0, 256);
        slot->poseByteOffset        = job.poseByteOffset;
        slot->vertexCount           = job.vertexCount;
        slot->outPosByteOffset      = job.outPosByteOffset;
        slot->outNrmByteOffset      = job.outNrmByteOffset;
        slot->morphCount            = job.morphCount;
        slot->morphWeightByteOffset = totalJobs * 512u;
        slot->prevPoseByteOffset    = job.prevPoseByteOffset;
        slot->outPrevPosByteOffset  = job.outPrevPosByteOffset;

        if (job.morphCount > 0 && m_morphWeightMapped)
        {
            float* wDst = reinterpret_cast<float*>(
                static_cast<uint8_t*>(m_morphWeightMapped) + totalJobs * 512u);
            std::memset(wDst, 0, 512);
            std::memcpy(wDst, job.morphWeights,
                        (std::min)(job.morphCount, 128u) * sizeof(float));
        }
        ++totalJobs;
    }

    if (totalJobs == 0) return cl;

    // ---- Phase 2 (GPU): bind shared resources once, dispatch per job ---------
    gfx.BindComputePipelineState(m_pso, cl);

    gfx.SetComputeDescriptorTable(kPose,   m_poseBuffer->GetCurrentSRVHandle(), cl);
    gfx.SetComputeDescriptorTable(kOutPos, m_vertRing->GetPosUAVHandle(),       cl);
    gfx.SetComputeDescriptorTable(kOutNrm, m_vertRing->GetNrmUAVHandle(),       cl);
    if (m_morphWeightSRV != 0)
        gfx.SetComputeDescriptorTable(kMorphWeights, m_morphWeightSRV, cl);

    uint32_t dispatchIdx = 0;
    for (const SkinDispatchDesc& job : *m_jobs)
    {
        if (job.vertexCount == 0) continue;
        if (dispatchIdx >= totalJobs) break;

        // Root CBV → point at this job's 256-byte slot in the upload buffer.
        gfx.SetComputeRootCBV(kCBSlot, m_jobBuffer, dispatchIdx * 256u, cl);

        gfx.SetComputeDescriptorTable(kRestPos, job.restPosSRVHandle, cl);
        gfx.SetComputeDescriptorTable(kRestNrm, job.restNrmSRVHandle, cl);
        gfx.SetComputeDescriptorTable(kBlend,   job.blendSRVHandle,   cl);

        if (job.morphDeltaSRVHandle != 0)
            gfx.SetComputeDescriptorTable(kMorphDeltas, job.morphDeltaSRVHandle, cl);

        gfx.DispatchCompute((job.vertexCount + 63) / 64, 1, 1, cl);
        ++dispatchIdx;
    }

    // UAV barrier: ensure all SkinningCS writes are visible before GBufferPass reads
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_vertRing->GetPosBuffer()), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_vertRing->GetNrmBuffer()), cl);

    return cl;
}
