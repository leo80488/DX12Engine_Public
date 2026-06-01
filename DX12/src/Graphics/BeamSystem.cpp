#include "Graphics/BeamSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MeshDescriptorHeap.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

namespace
{
    // Fill a CPU buffer with the canonical tube index pattern (16 axial × 12
    // radial × 6 = 1152 indices). Pattern repeats per beam-local vertex
    // window, so the same IB is shared across every beam slot — beams just
    // shift their position via MeshDescriptor.byteOffset.
    void BuildTubeIndices(uint32_t* out)
    {
        uint32_t idx = 0;
        for (uint32_t a = 0; a < BeamSystem::kAxialSegs; ++a)
        {
            const uint32_t row0 =  a      * BeamSystem::kRingVertCount;
            const uint32_t row1 = (a + 1) * BeamSystem::kRingVertCount;
            for (uint32_t r = 0; r < BeamSystem::kRadialSegs; ++r)
            {
                const uint32_t v00 = row0 + r;
                const uint32_t v10 = row0 + r + 1;
                const uint32_t v01 = row1 + r;
                const uint32_t v11 = row1 + r + 1;

                // Two CCW tris per quad. Tube outward-facing winding so that
                // backface-cull renders the visible side from outside.
                out[idx++] = v00;
                out[idx++] = v01;
                out[idx++] = v10;
                out[idx++] = v10;
                out[idx++] = v01;
                out[idx++] = v11;
            }
        }
    }
}

void BeamSystem::Init(IGraphicsDevice& gfx, MeshDescriptorHeap& meshDescHeap)
{
    m_gfx      = &gfx;
    m_meshHeap = &meshDescHeap;

    const uint64_t totalVerts = static_cast<uint64_t>(kMaxBeams) * kVertsPerBeam;

    auto createPVFBuffer = [&](RHI::GPUBuffer& dst, uint64_t bytesPerVert,
                               const char* name) -> bool
    {
        RHI::GPUBufferDesc d{};
        d.size       = totalVerts * bytesPerVert;
        d.usage      = RHI::Usage::DEFAULT;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(d, dst))
        {
            LOG_ERROR("BeamSystem: %s buffer creation failed", name);
            return false;
        }
        return true;
    };

    if (!createPVFBuffer(m_posBuffer,     12, "position")) return;
    if (!createPVFBuffer(m_normalBuffer,  12, "normal"))   return;
    if (!createPVFBuffer(m_tangentBuffer, 16, "tangent"))  return;
    if (!createPVFBuffer(m_uvBuffer,       8, "uv"))       return;

    m_posUav     = gfx.GetBufferUAVGpuHandle(m_posBuffer);
    m_normalUav  = gfx.GetBufferUAVGpuHandle(m_normalBuffer);
    m_tangentUav = gfx.GetBufferUAVGpuHandle(m_tangentBuffer);
    m_uvUav      = gfx.GetBufferUAVGpuHandle(m_uvBuffer);

    // Register as bindless ByteAddressBuffer SRVs so GBuffer.vs can fetch them.
    m_posBindless     = meshDescHeap.RegisterBuffer(m_posBuffer);
    m_normalBindless  = meshDescHeap.RegisterBuffer(m_normalBuffer);
    m_tangentBindless = meshDescHeap.RegisterBuffer(m_tangentBuffer);
    m_uvBindless      = meshDescHeap.RegisterBuffer(m_uvBuffer);

    // ---- Index buffer (DEFAULT, SR only) — populated at Init time ---------
    {
        std::vector<uint32_t> indexCpu(kIndicesPerBeam);
        BuildTubeIndices(indexCpu.data());

        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kIndicesPerBeam) * sizeof(uint32_t);
        d.usage      = RHI::Usage::DEFAULT;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(d, m_indexBuffer, indexCpu.data()))
        {
            LOG_ERROR("BeamSystem: index buffer creation failed");
            return;
        }
        m_indexBindless = meshDescHeap.RegisterBuffer(m_indexBuffer);
    }

    // ---- Control-point upload buffer (UPLOAD, SR StructuredBuffer) --------
    // Triple-buffered ring — written every frame by BeginFrame.
    {
        RHI::GPUBufferDesc d{};
        d.size = static_cast<uint64_t>(kMaxBeams) * kMaxControlPointsPerBeam
               * sizeof(BeamControlPointGPU);
        d.usage      = RHI::Usage::UPLOAD;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(BeamControlPointGPU);
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(d, m_controlPointsBuffer[i]))
            {
                m_controlPointsMapped[i] = gfx.MapBuffer(m_controlPointsBuffer[i]);
                m_controlPointsSrv[i]    = gfx.GetBufferSRVGpuHandle(m_controlPointsBuffer[i]);
            }
            if (!m_controlPointsMapped[i])
                LOG_ERROR("BeamSystem: control-points buffer map failed (slot %u)", i);
        }
    }

    // ---- Per-beam params CBV (UPLOAD, root CBV per dispatch) --------------
    // Triple-buffered ring — written every frame by BeginFrame.
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxBeams) * kBeamParamSlotStride;
        d.usage      = RHI::Usage::UPLOAD;
        d.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(d, m_paramsBuffer[i]))
                m_paramsMapped[i] = gfx.MapBuffer(m_paramsBuffer[i]);
            if (!m_paramsMapped[i])
                LOG_ERROR("BeamSystem: params buffer map failed (slot %u)", i);
        }
    }

    // ---- Pre-allocate kMaxBeams MeshDescriptor slots ----------------------
    // Each beam's descriptor points at the right offset slice of the shared
    // VBs; the IB indices are local 0..kVertsPerBeam-1 so they reach the
    // same window the VB offsets carved out.
    for (uint32_t i = 0; i < kMaxBeams; ++i)
    {
        const uint32_t vertBase = i * kVertsPerBeam;
        RHI::MeshDescriptor md{};
        md.position .bufferIndex = m_posBindless;
        md.position .byteOffset  = vertBase * 12;
        md.position .byteStride  = 12;
        md.position .format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

        md.normal   .bufferIndex = m_normalBindless;
        md.normal   .byteOffset  = vertBase * 12;
        md.normal   .byteStride  = 12;
        md.normal   .format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

        md.tangent  .bufferIndex = m_tangentBindless;
        md.tangent  .byteOffset  = vertBase * 16;
        md.tangent  .byteStride  = 16;
        md.tangent  .format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);

        md.uv0      .bufferIndex = m_uvBindless;
        md.uv0      .byteOffset  = vertBase * 8;
        md.uv0      .byteStride  = 8;
        md.uv0      .format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);

        // No second UV / no per-vertex color (unused by beam).
        md.uv1.bufferIndex = 0xFFFFFFFFu;

        md.indexBufferIndex = m_indexBindless;
        md.indexByteOffset  = 0;
        md.indexFormat      = 1;                 // uint32
        md.vertexCount      = kVertsPerBeam;

        m_perBeam[i].meshDescSlot = meshDescHeap.RegisterMesh(md);
    }

    // Build free list (highest slot first so Acquire returns 0 first).
    m_freeList.reserve(kMaxBeams);
    for (int i = static_cast<int>(kMaxBeams) - 1; i >= 0; --i)
        m_freeList.push_back(static_cast<uint32_t>(i));

    LOG_SUCCESS("BeamSystem: initialised (%u beams * %u verts * 48B = %zu KB)",
                kMaxBeams, kVertsPerBeam,
                (kMaxBeams * kVertsPerBeam * 48) / 1024);
}

void BeamSystem::Shutdown(IGraphicsDevice& gfx)
{
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_paramsMapped[i])        { gfx.UnmapBuffer(m_paramsBuffer[i]);        m_paramsMapped[i] = nullptr; }
        if (m_controlPointsMapped[i]) { gfx.UnmapBuffer(m_controlPointsBuffer[i]); m_controlPointsMapped[i] = nullptr; }
        if (m_paramsBuffer[i].IsValid())        gfx.DestroyBuffer(m_paramsBuffer[i]);
        if (m_controlPointsBuffer[i].IsValid()) gfx.DestroyBuffer(m_controlPointsBuffer[i]);
        m_controlPointsSrv[i] = 0;
    }
    if (m_indexBuffer.IsValid())         gfx.DestroyBuffer(m_indexBuffer);
    if (m_uvBuffer.IsValid())            gfx.DestroyBuffer(m_uvBuffer);
    if (m_tangentBuffer.IsValid())       gfx.DestroyBuffer(m_tangentBuffer);
    if (m_normalBuffer.IsValid())        gfx.DestroyBuffer(m_normalBuffer);
    if (m_posBuffer.IsValid())           gfx.DestroyBuffer(m_posBuffer);
    m_gfx = nullptr;
}

uint32_t BeamSystem::Acquire()
{
    if (m_freeList.empty()) return kInvalidBeamSlot;
    const uint32_t slot = m_freeList.back();
    m_freeList.pop_back();
    m_perBeam[slot].acquired         = true;
    m_perBeam[slot].hasControlPoints = false;
    m_perBeam[slot].controlPoints.clear();
    m_perBeam[slot].params           = {};
    return slot;
}

void BeamSystem::Release(uint32_t beamSlot)
{
    if (beamSlot >= kMaxBeams || !m_perBeam[beamSlot].acquired) return;
    m_perBeam[beamSlot].acquired = false;
    m_perBeam[beamSlot].hasControlPoints = false;
    m_freeList.push_back(beamSlot);
}

void BeamSystem::SetControlPoints(uint32_t beamSlot,
                                   const BeamControlPointGPU* points,
                                   uint32_t                   count)
{
    if (beamSlot >= kMaxBeams || !m_perBeam[beamSlot].acquired) return;

    const uint32_t clamped = std::min(count, kMaxControlPointsPerBeam);
    m_perBeam[beamSlot].controlPoints.assign(points, points + clamped);
    m_perBeam[beamSlot].hasControlPoints = (clamped >= 2);
}

void BeamSystem::SetParams(uint32_t beamSlot, const BeamGenParamsGPU& params)
{
    if (beamSlot >= kMaxBeams || !m_perBeam[beamSlot].acquired) return;
    m_perBeam[beamSlot].params = params;
}

void BeamSystem::BeginFrame(float globalTimeSec)
{
    m_activeBeams.clear();

    if (!m_gfx) return;
    const uint32_t frameSlot = m_gfx->GetFrameIndex();
    if (frameSlot >= kFrameCount) return;
    if (!m_controlPointsMapped[frameSlot] || !m_paramsMapped[frameSlot]) return;

    auto* cpDst     = static_cast<BeamControlPointGPU*>(m_controlPointsMapped[frameSlot]);
    auto* paramsDst = static_cast<uint8_t*>(m_paramsMapped[frameSlot]);

    for (uint32_t i = 0; i < kMaxBeams; ++i)
    {
        const PerBeam& pb = m_perBeam[i];
        if (!pb.acquired || !pb.hasControlPoints) continue;

        const uint32_t cpOffset = i * kMaxControlPointsPerBeam;
        std::memcpy(cpDst + cpOffset,
                    pb.controlPoints.data(),
                    pb.controlPoints.size() * sizeof(BeamControlPointGPU));

        // Patch the offsets so the CS reads its own slice.
        BeamGenParamsGPU p = pb.params;
        p.controlPointOffset = cpOffset;
        p.controlPointCount  = static_cast<uint32_t>(pb.controlPoints.size());
        p.vertexBaseElement  = i * kVertsPerBeam;
        p.time               = globalTimeSec;
        std::memcpy(paramsDst + i * kBeamParamSlotStride, &p, sizeof(p));

        m_activeBeams.push_back({ i, static_cast<uint32_t>(i * kBeamParamSlotStride) });
    }
}

// ---------------------------------------------------------------------------
// Per-frame accessors — return the slot matching gfx.GetFrameIndex().
const RHI::GPUBuffer& BeamSystem::GetParamsBuffer() const
{
    const uint32_t s = m_gfx ? m_gfx->GetFrameIndex() : 0;
    return m_paramsBuffer[s < kFrameCount ? s : 0];
}

const RHI::GPUBuffer& BeamSystem::GetControlPointsBuf() const
{
    const uint32_t s = m_gfx ? m_gfx->GetFrameIndex() : 0;
    return m_controlPointsBuffer[s < kFrameCount ? s : 0];
}

uint64_t BeamSystem::GetControlPointsSrv() const
{
    if (!m_gfx) return 0;
    const uint32_t s = m_gfx->GetFrameIndex();
    return s < kFrameCount ? m_controlPointsSrv[s] : 0;
}
