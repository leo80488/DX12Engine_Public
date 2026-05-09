#pragma once

// BeamSystem — GPU-driven procedural-tube beam (heavy-beam) generator.
//
// All beams share five PVF buffers (pos / normal / tangent / uv + index)
// allocated as a single big slab; per-beam vertex windows are addressed via
// MeshDescriptor.byteOffset. A single CS dispatch per beam writes that
// beam's slice each frame, computing the parallel-transport frame so
// curved beams don't twist.
//
// Typical scene: 1..10 active beams; pool sized at 32 with headroom.
//
// Lifecycle:
//   uint32_t slot = system->Acquire();
//   system->SetControlPoints(slot, points);
//   system->SetParams(slot, { .globalRadiusScale = 1.0f, .wobbleAmplitude = 0.02f });
//   ... draw via DrawPacket{ meshDescriptorIndex = system->GetMeshDescSlot(slot), ... }
//   system->Release(slot);

#include "Graphics/GraphicsStruct.h"

#include <vector>
#include <cstdint>
#include <DirectXMath.h>

class IGraphicsDevice;
class MeshDescriptorHeap;

// 32 bytes — must match shaders/BeamCommon.hlsli BeamControlPoint.
struct alignas(16) BeamControlPointGPU
{
    DirectX::XMFLOAT3 position;
    float             radius;
    DirectX::XMFLOAT4 colorTint;
};
static_assert(sizeof(BeamControlPointGPU) == 32, "BeamControlPointGPU layout drift");

// 256 bytes — per-beam CS CBV slot.  The HLSL side (BeamGenParams) only
// reads the first ~32 bytes of meaningful data; the rest is padding to
// satisfy D3D12's 256-byte CBV alignment requirement.
struct alignas(16) BeamGenParamsGPU
{
    uint32_t controlPointOffset;   // 4 — index into shared CP buffer
    uint32_t controlPointCount;    // 4 — typically 2..8
    uint32_t vertexBaseElement;    // 4 — start vertex index in shared VBs
    uint32_t _pad0;                // 4
    float    globalRadiusScale;    // 4
    float    wobbleAmplitude;      // 4
    float    wobbleSpeed;          // 4
    float    time;                 // 4
    uint8_t  _pad1[256 - 32];      // padding to a full 256B CBV slot
};
static_assert(sizeof(BeamGenParamsGPU) == 256, "BeamGenParamsGPU must be 256B");

class BeamSystem
{
public:
    // Tube topology — must match shaders/BeamCommon.hlsli macros.
    static constexpr uint32_t kRadialSegs                 = 12;
    static constexpr uint32_t kAxialSegs                  = 16;
    static constexpr uint32_t kRingVertCount              = kRadialSegs + 1;       // +1 for UV seam
    static constexpr uint32_t kAxialRingCount             = kAxialSegs + 1;
    static constexpr uint32_t kVertsPerBeam               = kRingVertCount * kAxialRingCount;     // 13×17 = 221
    static constexpr uint32_t kIndicesPerBeam             = kAxialSegs * kRadialSegs * 6;          // 16×12×6 = 1152
    static constexpr uint32_t kMaxControlPointsPerBeam    = 8;

    // Pool size. 32 × 221 verts × (12+12+16+8 = 48 B) ≈ 340 KB — cheap.
    static constexpr uint32_t kMaxBeams                   = 32;
    static constexpr uint64_t kBeamParamSlotStride        = 256;

    static constexpr uint32_t kInvalidBeamSlot            = 0xFFFFFFFFu;

    void Init(IGraphicsDevice& gfx, MeshDescriptorHeap& meshDescHeap);
    void Shutdown(IGraphicsDevice& gfx);

    // ---- Beam slot management -----------------------------------------------
    // Returns a beam slot index in [0, kMaxBeams) or kInvalidBeamSlot if pool
    // is exhausted. The slot is reserved until Release() is called.
    uint32_t Acquire();
    void     Release(uint32_t beamSlot);

    // Replace this beam's control-point list. `points` is at most
    // kMaxControlPointsPerBeam entries; extras are dropped.
    void SetControlPoints(uint32_t beamSlot,
                          const BeamControlPointGPU* points,
                          uint32_t                   count);

    // Replace this beam's per-frame params (radius scale / wobble / time).
    // CS uses these values in its next dispatch.
    void SetParams(uint32_t beamSlot, const BeamGenParamsGPU& params);

    // Mesh descriptor slot for this beam — pass into DrawPacket.meshDescriptorIndex.
    // Stable for the slot's whole life; never invalidates.
    uint32_t GetMeshDescSlot(uint32_t beamSlot) const
    { return beamSlot < kMaxBeams ? m_perBeam[beamSlot].meshDescSlot : 0xFFFFFFFFu; }

    // Index count for one beam — used by DrawIndexedInstanced(rawCount = ...).
    static constexpr uint32_t GetIndexCount() { return kIndicesPerBeam; }

    // ---- BeamSimPass-facing accessors --------------------------------------
    // Used by BeamSimPass to set up per-beam dispatches.
    struct ActiveBeam
    {
        uint32_t beamSlot;
        uint32_t cbOffset;   // bytes into m_paramsBuffer for this beam's CBV slot
    };
    const std::vector<ActiveBeam>& GetActiveBeams() const { return m_activeBeams; }

    const RHI::GPUBuffer& GetParamsBuffer()    const { return m_paramsBuffer; }
    const RHI::GPUBuffer& GetControlPointsBuf()const { return m_controlPointsBuffer; }
    uint64_t              GetControlPointsSrv()const { return m_controlPointsSrv; }

    const RHI::GPUBuffer& GetPosBuffer()       const { return m_posBuffer; }
    const RHI::GPUBuffer& GetNormalBuffer()    const { return m_normalBuffer; }
    const RHI::GPUBuffer& GetTangentBuffer()   const { return m_tangentBuffer; }
    const RHI::GPUBuffer& GetUvBuffer()        const { return m_uvBuffer; }

    uint64_t GetPosUav()     const { return m_posUav; }
    uint64_t GetNormalUav()  const { return m_normalUav; }
    uint64_t GetTangentUav() const { return m_tangentUav; }
    uint64_t GetUvUav()      const { return m_uvUav; }

    // Refresh m_activeBeams + upload latest CPU control-points / params for
    // every active beam. Call from Renderer::BeginFrame; consumed by
    // BeamSimPass::Execute later in the frame.
    // `globalTimeSec` is forwarded into each beam's params.time for the
    // wobble noise — saves every caller having to inject it manually.
    void BeginFrame(float globalTimeSec);

private:
    IGraphicsDevice*    m_gfx     = nullptr;
    MeshDescriptorHeap* m_meshHeap = nullptr;

    // Shared PVF VBs (DEFAULT heap, SRV + UAV, ByteAddressBuffer).
    RHI::GPUBuffer m_posBuffer;       // 12 B/vert
    RHI::GPUBuffer m_normalBuffer;    // 12 B/vert
    RHI::GPUBuffer m_tangentBuffer;   // 16 B/vert
    RHI::GPUBuffer m_uvBuffer;        //  8 B/vert
    uint64_t       m_posUav     = 0, m_normalUav = 0, m_tangentUav = 0, m_uvUav = 0;
    uint32_t       m_posBindless = 0, m_normalBindless = 0,
                   m_tangentBindless = 0, m_uvBindless = 0;

    // Shared IB (DEFAULT heap, ByteAddressBuffer SRV — no UAV needed; CPU
    // fills once at Init with the canonical tube index pattern, then read by
    // every beam draw).
    RHI::GPUBuffer m_indexBuffer;
    uint32_t       m_indexBindless = 0;

    // Per-beam control-point buffer (UPLOAD heap, SR-bound StructuredBuffer).
    RHI::GPUBuffer m_controlPointsBuffer;
    uint64_t       m_controlPointsSrv    = 0;
    void*          m_controlPointsMapped = nullptr;

    // Per-beam BeamGenParams CBV (UPLOAD heap, root CBV per dispatch with
    // offset = beamSlot * kBeamParamSlotStride).
    RHI::GPUBuffer m_paramsBuffer;
    void*          m_paramsMapped = nullptr;

    struct PerBeam
    {
        bool                                       acquired = false;
        bool                                       hasControlPoints = false;
        uint32_t                                   meshDescSlot = 0xFFFFFFFFu;
        std::vector<BeamControlPointGPU>           controlPoints;       // CPU mirror
        BeamGenParamsGPU                           params{};            // CPU mirror
    };
    PerBeam               m_perBeam[kMaxBeams];
    std::vector<uint32_t> m_freeList;            // beam slot indices

    // Built each BeginFrame; consumed by BeamSimPass.
    std::vector<ActiveBeam> m_activeBeams;
};
