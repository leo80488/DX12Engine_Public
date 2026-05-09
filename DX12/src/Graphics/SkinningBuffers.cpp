#include "Graphics/SkinningBuffers.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cassert>
#include <cstring>

// =============================================================================
// PoseRingBuffer
// =============================================================================

void PoseRingBuffer::Init(IGraphicsDevice& gfx)
{
    const uint64_t bufSize = static_cast<uint64_t>(kMaxBonesPerFrame) * sizeof(DirectX::XMFLOAT4X4);

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = bufSize;
        bd.stride     = sizeof(DirectX::XMFLOAT4X4);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW; // ByteAddressBuffer

        if (!gfx.CreateBuffer(bd, m_buffers[i]))
        {
            LOG_ERROR("PoseRingBuffer: failed to create buffer slot %u", i);
            continue;
        }

        m_mapped[i]     = gfx.MapBuffer(m_buffers[i]);
        m_srvHandles[i] = gfx.GetBufferSRVGpuHandle(m_buffers[i]);

        if (!m_mapped[i])
            LOG_ERROR("PoseRingBuffer: MapBuffer failed for slot %u", i);
        if (!m_srvHandles[i])
            LOG_WARNING("PoseRingBuffer: SRV handle is 0 for slot %u — check bind_flags", i);
    }

    LOG_SUCCESS("PoseRingBuffer: initialised (%u KB × %u frames)",
                static_cast<uint32_t>(bufSize / 1024), kFramesInFlight);
}

void PoseRingBuffer::BeginFrame(uint64_t frameIndex)
{
    m_frameSlot = static_cast<uint32_t>(frameIndex % kFramesInFlight);
    m_writeHead = 0;
}

uint32_t PoseRingBuffer::Alloc(uint32_t boneCount)
{
    const uint32_t slot = m_writeHead;
    m_writeHead += boneCount;
    assert(m_writeHead <= kMaxBonesPerFrame && "PoseRingBuffer overflow — increase kMaxBonesPerFrame");
    return slot;
}

DirectX::XMFLOAT4X4* PoseRingBuffer::MapSlice(uint32_t slot, uint32_t /*boneCount*/)
{
    if (!m_mapped[m_frameSlot]) return nullptr;
    return static_cast<DirectX::XMFLOAT4X4*>(m_mapped[m_frameSlot]) + slot;
}

uint64_t PoseRingBuffer::GetCurrentSRVHandle() const
{
    return m_srvHandles[m_frameSlot];
}

const RHI::GPUBuffer& PoseRingBuffer::GetCurrentBuffer() const
{
    return m_buffers[m_frameSlot];
}

const DirectX::XMFLOAT4X4* PoseRingBuffer::ReadMapped(uint32_t byteOffset) const
{
    if (!m_mapped[m_frameSlot]) return nullptr;
    const auto* base = static_cast<const DirectX::XMFLOAT4X4*>(m_mapped[m_frameSlot]);
    return base + (byteOffset / sizeof(DirectX::XMFLOAT4X4));
}

// =============================================================================
// SkinnedVertexRing
// =============================================================================

void SkinnedVertexRing::Init(IGraphicsDevice& gfx)
{
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        posBindlessIdx[i]     = RHI::kInvalidBufferIndex;
        nrmBindlessIdx[i]     = RHI::kInvalidBufferIndex;
        prevPosBindlessIdx[i] = RHI::kInvalidBufferIndex;
    }

    // Position buffer: float3 per vertex = 12 bytes
    const uint64_t posBufSize = static_cast<uint64_t>(kMaxVertsPerFrame) * sizeof(float) * 3;
    // Normal buffer: float3 per vertex = 12 bytes
    const uint64_t nrmBufSize = static_cast<uint64_t>(kMaxVertsPerFrame) * sizeof(float) * 3;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        // Position buffer — UAV (CS writes) + SRV (VS reads)
        // Structured float3 buffer (stride=12); NOT raw so existing CreateBuffer creates correct UAV.
        {
            RHI::GPUBufferDesc bd{};
            bd.size       = posBufSize;
            bd.stride     = sizeof(float) * 3; // structured float3
            bd.usage      = RHI::Usage::DEFAULT;
            bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
            bd.misc_flags = RHI::ResourceMiscFlag::NONE; // structured, not raw

            if (!gfx.CreateBuffer(bd, m_posBuffers[i]))
            {
                LOG_ERROR("SkinnedVertexRing: failed to create pos buffer slot %u", i);
                continue;
            }

            m_posUAVHandles[i] = gfx.GetBufferUAVGpuHandle(m_posBuffers[i]);
            m_posSRVHandles[i] = gfx.GetBufferSRVGpuHandle(m_posBuffers[i]);
        }

        // Normal buffer — UAV (CS writes) + SRV (VS reads), structured float3
        {
            RHI::GPUBufferDesc bd{};
            bd.size       = nrmBufSize;
            bd.stride     = sizeof(float) * 3;
            bd.usage      = RHI::Usage::DEFAULT;
            bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
            bd.misc_flags = RHI::ResourceMiscFlag::NONE;

            if (!gfx.CreateBuffer(bd, m_nrmBuffers[i]))
            {
                LOG_ERROR("SkinnedVertexRing: failed to create nrm buffer slot %u", i);
                continue;
            }

            m_nrmUAVHandles[i] = gfx.GetBufferUAVGpuHandle(m_nrmBuffers[i]);
            m_nrmSRVHandles[i] = gfx.GetBufferSRVGpuHandle(m_nrmBuffers[i]);
        }

        // Previous-frame position buffer (for TAA velocity on skinned meshes)
        {
            RHI::GPUBufferDesc bd{};
            bd.size       = posBufSize;
            bd.stride     = sizeof(float) * 3;
            bd.usage      = RHI::Usage::DEFAULT;
            bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
            bd.misc_flags = RHI::ResourceMiscFlag::NONE;

            if (!gfx.CreateBuffer(bd, m_prevPosBuffers[i]))
                LOG_ERROR("SkinnedVertexRing: failed to create prevPos buffer slot %u", i);
            else
            {
                m_prevPosUAVHandles[i] = gfx.GetBufferUAVGpuHandle(m_prevPosBuffers[i]);
                m_prevPosSRVHandles[i] = gfx.GetBufferSRVGpuHandle(m_prevPosBuffers[i]);
            }
        }
    }

    LOG_SUCCESS("SkinnedVertexRing: initialised (pos %u KB, nrm %u KB, prevPos %u KB x %u frames)",
                static_cast<uint32_t>(posBufSize / 1024),
                static_cast<uint32_t>(nrmBufSize / 1024),
                static_cast<uint32_t>(posBufSize / 1024),
                kFramesInFlight);
}

void SkinnedVertexRing::BeginFrame(uint64_t frameIndex)
{
    m_frameSlot       = static_cast<uint32_t>(frameIndex % kFramesInFlight);
    m_posByteHead     = 0;
    m_nrmByteHead     = 0;
    m_prevPosByteHead = 0;
}

uint32_t SkinnedVertexRing::AllocPosition(uint32_t vertexCount)
{
    const uint32_t byteOffset = m_posByteHead;
    m_posByteHead += vertexCount * sizeof(float) * 3; // 12 bytes/vertex
    assert(m_posByteHead <= kMaxVertsPerFrame * sizeof(float) * 3
           && "SkinnedVertexRing position overflow");
    return byteOffset;
}

uint32_t SkinnedVertexRing::AllocNormal(uint32_t vertexCount)
{
    const uint32_t byteOffset = m_nrmByteHead;
    m_nrmByteHead += vertexCount * sizeof(float) * 3; // 12 bytes/vertex
    assert(m_nrmByteHead <= kMaxVertsPerFrame * sizeof(float) * 3
           && "SkinnedVertexRing normal overflow");
    return byteOffset;
}

uint64_t SkinnedVertexRing::GetPosUAVHandle()     const { return m_posUAVHandles[m_frameSlot]; }
uint64_t SkinnedVertexRing::GetNrmUAVHandle()     const { return m_nrmUAVHandles[m_frameSlot]; }
uint64_t SkinnedVertexRing::GetPrevPosUAVHandle() const { return m_prevPosUAVHandles[m_frameSlot]; }
uint64_t SkinnedVertexRing::GetPosSRVHandle()     const { return m_posSRVHandles[m_frameSlot]; }
uint64_t SkinnedVertexRing::GetNrmSRVHandle()     const { return m_nrmSRVHandles[m_frameSlot]; }
uint64_t SkinnedVertexRing::GetPrevPosSRVHandle() const { return m_prevPosSRVHandles[m_frameSlot]; }

const RHI::GPUBuffer& SkinnedVertexRing::GetPosBuffer()     const { return m_posBuffers[m_frameSlot]; }
const RHI::GPUBuffer& SkinnedVertexRing::GetNrmBuffer()     const { return m_nrmBuffers[m_frameSlot]; }
const RHI::GPUBuffer& SkinnedVertexRing::GetPrevPosBuffer() const { return m_prevPosBuffers[m_frameSlot]; }

uint32_t SkinnedVertexRing::AllocPrevPosition(uint32_t vertexCount)
{
    const uint32_t byteOffset = m_prevPosByteHead;
    m_prevPosByteHead += vertexCount * sizeof(float) * 3;
    assert(m_prevPosByteHead <= kMaxVertsPerFrame * sizeof(float) * 3
           && "SkinnedVertexRing prevPos overflow");
    return byteOffset;
}
