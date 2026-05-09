#pragma once

// PoseRingBuffer    — per-frame UPLOAD ring buffer holding bone matrices for all skinned entities.
//                     CPU writes float4x4[] via MapSlice(); GPU reads in SkinningCS.
//
// SkinnedVertexRing — per-frame DEFAULT ring buffers for SkinningCS output.
//                     posBuffer: float3[] positions (UAV write, SRV read by VS)
//                     nrmBuffer: float3[] normals   (UAV write, SRV read by VS)
//                     Both buffers stay alive for the duration of the frame; barriers
//                     are managed by the Renderer / SkinningPass.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/GraphicsStruct.h"
#include <cstdint>
#include <DirectXMath.h>

class IGraphicsDevice;

// =============================================================================
// PoseRingBuffer
// =============================================================================
class PoseRingBuffer
{
public:
    // Enough for characters + TAA prev pose copies (×2 per skinned entity).
    static constexpr uint32_t kMaxBonesPerFrame = 1 << 20;
    static constexpr uint32_t kFramesInFlight   = 3;

    // Call once after device creation.
    void Init(IGraphicsDevice& gfx);

    // Reset the per-frame write head; call at the start of every BeginFrame.
    // frameIndex wraps internally via % kFramesInFlight.
    void BeginFrame(uint64_t frameIndex);

    // Allocate boneCount consecutive matrix slots; returns the first slot index.
    // Slots are valid until the next BeginFrame() for the same frame-in-flight slot.
    uint32_t Alloc(uint32_t boneCount);

    // Returns a CPU-writable pointer to boneCount matrices starting at 'slot'.
    // Do NOT write past slot + boneCount.
    DirectX::XMFLOAT4X4* MapSlice(uint32_t slot, uint32_t boneCount);

    // GPU SRV handle for the current frame's buffer (ByteAddressBuffer for SkinningCS).
    uint64_t GetCurrentSRVHandle() const;

    // The current frame's GPUBuffer (used to emit barriers if needed).
    const RHI::GPUBuffer& GetCurrentBuffer() const;

    // Read-only access to the current frame's mapped bone matrices (for debug).
    // Returns nullptr if the buffer is not mapped.  byteOffset / 64 = first bone index.
    const DirectX::XMFLOAT4X4* ReadMapped(uint32_t byteOffset) const;
    uint32_t GetWriteHead() const { return m_writeHead; }

private:
    RHI::GPUBuffer m_buffers[kFramesInFlight];
    void*          m_mapped[kFramesInFlight]{};
    uint64_t       m_srvHandles[kFramesInFlight]{};

    uint32_t m_writeHead = 0;  // current bone slot cursor (resets each BeginFrame)
    uint32_t m_frameSlot = 0;  // current index into [kFramesInFlight]
};

// =============================================================================
// SkinnedVertexRing
// =============================================================================
class SkinnedVertexRing
{
public:
    // Enough for ~4 M vertices across all visible skinned entities.
    // Each skinned entity allocates position + normal + TAA prev-position.
    static constexpr uint32_t kMaxVertsPerFrame = 1 << 22; 
    static constexpr uint32_t kFramesInFlight   = 3;

    // Call once; the Renderer must subsequently call RegisterBuffer() on each
    // frame slot to fill posBindlessIdx[] / nrmBindlessIdx[].
    void Init(IGraphicsDevice& gfx);

    // Reset per-frame allocation heads; call at the start of every BeginFrame.
    void BeginFrame(uint64_t frameIndex);

    // Allocate vertexCount position slots; returns the BYTE OFFSET into posBuffer.
    uint32_t AllocPosition(uint32_t vertexCount);

    // Allocate vertexCount normal slots; returns the BYTE OFFSET into nrmBuffer.
    uint32_t AllocNormal(uint32_t vertexCount);

    // UAV GPU handles — bind to compute shader for write.
    uint64_t GetPosUAVHandle() const;
    uint64_t GetNrmUAVHandle() const;
    uint64_t GetPrevPosUAVHandle() const;

    // SRV GPU handles — bind to GBuffer VS for read (valid after UAV barrier).
    uint64_t GetPosSRVHandle() const;
    uint64_t GetNrmSRVHandle() const;
    uint64_t GetPrevPosSRVHandle() const;

    // Current frame's GPU buffers (for barrier emission).
    const RHI::GPUBuffer& GetPosBuffer() const;
    const RHI::GPUBuffer& GetNrmBuffer() const;
    const RHI::GPUBuffer& GetPrevPosBuffer() const;

    // Allocate vertexCount previous-position slots; returns the BYTE OFFSET.
    uint32_t AllocPrevPosition(uint32_t vertexCount);

    // Bindless indices in g_Buffers[] for each frame slot.
    // Set by Renderer::Compile via RegisterBuffer(); use GetCurrentPosBindlessIdx() each frame.
    uint32_t posBindlessIdx[kFramesInFlight];
    uint32_t nrmBindlessIdx[kFramesInFlight];
    uint32_t prevPosBindlessIdx[kFramesInFlight];

    uint32_t GetCurrentPosBindlessIdx()     const { return posBindlessIdx[m_frameSlot]; }
    uint32_t GetCurrentNrmBindlessIdx()     const { return nrmBindlessIdx[m_frameSlot]; }
    uint32_t GetCurrentPrevPosBindlessIdx() const { return prevPosBindlessIdx[m_frameSlot]; }

private:
    RHI::GPUBuffer m_posBuffers[kFramesInFlight];
    RHI::GPUBuffer m_nrmBuffers[kFramesInFlight];
    RHI::GPUBuffer m_prevPosBuffers[kFramesInFlight];

    uint64_t m_posUAVHandles[kFramesInFlight]{};
    uint64_t m_nrmUAVHandles[kFramesInFlight]{};
    uint64_t m_prevPosUAVHandles[kFramesInFlight]{};
    uint64_t m_posSRVHandles[kFramesInFlight]{};
    uint64_t m_nrmSRVHandles[kFramesInFlight]{};
    uint64_t m_prevPosSRVHandles[kFramesInFlight]{};

    uint32_t m_posByteHead     = 0;
    uint32_t m_nrmByteHead     = 0;
    uint32_t m_prevPosByteHead = 0;
    uint32_t m_frameSlot       = 0;
};
