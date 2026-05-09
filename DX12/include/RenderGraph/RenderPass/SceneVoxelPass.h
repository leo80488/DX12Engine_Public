#pragma once

// SceneVoxelPass — triangle-precision world-space occupancy grid for the
// volumetric fog LightInject shader. Each frame:
//
//   1. A compute clear zeroes the 3D R8_UINT occupancy texture.
//   2. For each opaque draw packet in the list set by Renderer, a compute
//      dispatch runs one thread per triangle, fetches its vertices via PVF,
//      transforms to world, and marks every voxel the triangle actually
//      intersects (Akenine-Möller triangle-box SAT).
//
// That produces a grid where occupied cells correspond to real geometry —
// doorways / openings stay empty, thin occluders register only where the
// triangles actually lie, and concave meshes leave their interior voids
// unmarked. LightInject raymarches the grid from voxel → light and treats
// any cell hit as full occlusion.
//
// Tier 2 of three: correct per-triangle precision via compute SAT. A future
// Tier 3 would replace the per-triangle SAT loop with conservative
// rasterization for a further ~2-3× runtime speedup; the visual result is
// identical.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

class SceneVoxelPass : public RG::RenderPass
{
public:
    static constexpr uint32_t kGridDim  = 128;
    // Max independent mesh dispatches per frame. Each consumes one 256-byte
    // slot in the per-dispatch CB. D3D12 caps a single CBV descriptor at
    // 64 KB = 256 slots; slot 0 is reserved for the clear pass, leaving
    // kMaxDraws for per-mesh voxelize dispatches. Scenes needing more opaque
    // meshes per frame should either sort by size (skip tiny ones) or extend
    // this with a second CB ring.
    static constexpr uint32_t kMaxDraws = 255;

    const char* GetName() const override { return "SceneVoxelPass"; }
    void Setup (RG::RenderGraphBuilder&) override {}
    void Init  (IGraphicsDevice& gfx)    override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // World-space AABB of the occupancy grid. Renderer snaps this to voxel-
    // sized units centered on the camera each frame.
    void SetGridBounds(const DirectX::XMFLOAT3& mn, const DirectX::XMFLOAT3& mx)
    {
        m_gridMin = mn;
        m_gridMax = mx;
    }

    // Resource handles the voxelize shader needs. Renderer sets these once
    // per frame after BuildRenderScene so pointers are stable.
    void SetSceneBindings(uint64_t instanceBufferSrv,
                          uint64_t meshDescriptorsSrv,
                          uint64_t bindlessBufferTable)
    {
        m_instanceBufferSrv    = instanceBufferSrv;
        m_meshDescriptorsSrv   = meshDescriptorsSrv;
        m_bindlessBufferTable  = bindlessBufferTable;
    }

    // ---- Per-frame draw list --------------------------------------------------
    //
    // Call ResetDraws() once per frame, then PushDraw() for every opaque mesh
    // whose bounding box overlaps the grid. Execute() consumes the list.
    struct DrawEntry
    {
        uint32_t meshDescIdx;
        uint32_t instanceOffset;
        uint32_t triangleCount;
    };
    void ResetDraws() { m_draws.clear(); }
    void PushDraw(uint32_t meshDescIdx, uint32_t instanceOffset, uint32_t triangleCount)
    {
        if (triangleCount == 0) return;
        if (m_draws.size() >= kMaxDraws) return;
        m_draws.push_back({ meshDescIdx, instanceOffset, triangleCount });
    }
    uint32_t GetDrawCount() const { return static_cast<uint32_t>(m_draws.size()); }

    // ---- Consumed by VolumetricFogPass ---------------------------------------
    uint64_t                  GetOccupancySrvHandle() const;
    const DirectX::XMFLOAT3&  GetGridMin() const { return m_gridMin; }
    const DirectX::XMFLOAT3&  GetGridMax() const { return m_gridMax; }
    static constexpr uint32_t GetGridDim()       { return kGridDim; }

private:
    // Per-dispatch CB for the voxelize shader. 256-byte aligned so we can
    // pack many into one UPLOAD buffer and advance the CBV root offset
    // between dispatches without aliasing.
    struct alignas(16) VoxelizeCB
    {
        float    gridMin[3];     float _pad0;
        float    gridExtent[3];  uint32_t gridDim;
        uint32_t meshDescIdx;
        uint32_t instanceOffset;
        uint32_t numTriangles;
        uint32_t _pad1;
    };

    // Clear shader's CB. Separate slot in the same buffer, kept simple.
    struct alignas(16) ClearCB
    {
        uint32_t gridDim;
        uint32_t _pad[3];
    };

    static constexpr uint32_t kCBSlotStride = 256; // D3D12 CBV alignment
    static_assert(sizeof(VoxelizeCB) <= kCBSlotStride, "VoxelizeCB overflows CB slot");
    static_assert(sizeof(ClearCB)    <= kCBSlotStride, "ClearCB overflows CB slot");

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_voxelizePSO;
    RHI::PipelineState m_clearPSO;

    // Occupancy grid (Texture3D<uint> R8_UINT).
    RHI::Texture       m_occupancyTex;
    RHI::ResourceState m_occupancyState = RHI::ResourceState::UNORDERED_ACCESS;

    // Per-frame CB ring — enough for kMaxDraws slots + 1 clear slot at slot 0.
    RHI::GPUBuffer     m_cb;
    void*              m_cbMapped = nullptr;

    DirectX::XMFLOAT3  m_gridMin { -32.f, -32.f, -32.f };
    DirectX::XMFLOAT3  m_gridMax {  32.f,  32.f,  32.f };

    // Resource bindings — SRVs / bindless tables Renderer stamps in each frame.
    uint64_t m_instanceBufferSrv   = 0;
    uint64_t m_meshDescriptorsSrv  = 0;
    uint64_t m_bindlessBufferTable = 0;

    std::vector<DrawEntry> m_draws;
};
