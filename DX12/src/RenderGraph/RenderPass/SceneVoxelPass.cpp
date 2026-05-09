#include "RenderGraph/RenderPass/SceneVoxelPass.h"

#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>

// Compute root sig slots (must match GraphicsDX12::CreateComputeRootSignature):
//   [0]  ROOT_CBV   b0 space2 — VoxelizeCB / ClearCB
//   [4]  DESC_TABLE u0 space2 — RWTexture3D<uint> occupancy
//   [9]  DESC_TABLE t0 space0 — StructuredBuffer<GPUInstanceData>
//   [10] DESC_TABLE t1 space0 — StructuredBuffer<MeshDescriptor>
//   [11] DESC_TABLE t0 space1 — bindless g_Buffers[] (ByteAddressBuffer array)
static constexpr uint32_t kCBSlot         = 0;
static constexpr uint32_t kUAVSlot        = 4;
static constexpr uint32_t kInstanceSlot   = 9;
static constexpr uint32_t kMeshDescSlot   = 10;
static constexpr uint32_t kBindlessSlot   = 11;

// ---------------------------------------------------------------------------
void SceneVoxelPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SceneVoxelize_CS,  RHI::ShaderStage::CS,
                         "SceneVoxelize.cs.hlsl",  "CSMain");
    m_shaderLib.Register(ShaderID::SceneVoxelClear_CS, RHI::ShaderStage::CS,
                         "SceneVoxelClear.cs.hlsl", "CSMain");

    auto makeCs = [&](ShaderID id, RHI::PipelineState& out, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("SceneVoxelPass: %s missing", tag); return; }
        RHI::PipelineStateDesc pd{};
        pd.cs = cs;
        if (!gfx.CreatePipelineState(pd, out))
            LOG_ERROR("SceneVoxelPass: %s PSO create failed", tag);
    };
    makeCs(ShaderID::SceneVoxelize_CS,  m_voxelizePSO, "voxelize");
    makeCs(ShaderID::SceneVoxelClear_CS, m_clearPSO,   "clear");

    // ---- Occupancy texture (128³ R8_UINT, 2 MB) ---------------------------
    {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
        td.width      = kGridDim;
        td.height     = kGridDim;
        td.depth      = kGridDim;
        td.mip_levels = 1;
        td.format     = RHI::Format::R8_UINT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!gfx.CreateTexture(td, m_occupancyTex))
            LOG_ERROR("SceneVoxelPass: occupancy texture create failed");
    }

    // ---- Per-dispatch CB (ring of kMaxDraws+1 slots) ----------------------
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kCBSlotStride) * (kMaxDraws + 1);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_cb))
            m_cbMapped = gfx.MapBuffer(m_cb);
    }

    LOG_SUCCESS("SceneVoxelPass: Tier-2 triangle voxelization initialised (%u^3 R8_UINT)",
                kGridDim);
}

// ---------------------------------------------------------------------------
uint64_t SceneVoxelPass::GetOccupancySrvHandle() const
{
    if (!m_gfx || !m_occupancyTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_occupancyTex);
}

// ---------------------------------------------------------------------------
RHI::CommandList SceneVoxelPass::Execute(RHI::CommandList cl)
{
    if (!m_voxelizePSO.IsValid() || !m_clearPSO.IsValid()) return cl;
    if (!m_occupancyTex.IsValid() || !m_cbMapped)          return cl;

    // Nothing to voxelise this frame (e.g. volumetric fog disabled → Renderer
    // skipped SetSceneBindings / PushDraw). Skip the clear too — the grid's
    // content is moot while no consumer samples it, and VolumetricFogPass sees
    // a zero occupancy SRV handle in that state so LightInject falls through
    // its "no grid" branch regardless of what's in the texture.
    if (m_draws.empty()) return cl;

    IGraphicsDevice& gfx = *m_gfx;

    // Make the 3D texture UAV-writable (it comes back in SR_COMPUTE from the
    // previous frame's LightInject read).
    if (m_occupancyState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_occupancyTex, m_occupancyState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_occupancyState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    const uint64_t occupancyUav = gfx.GetTextureUAVGpuHandle(m_occupancyTex);

    // ---- Phase 1: clear the grid -------------------------------------------
    {
        ClearCB cc{};
        cc.gridDim = kGridDim;
        uint8_t* dst = static_cast<uint8_t*>(m_cbMapped); // slot 0 = clear
        std::memcpy(dst, &cc, sizeof(cc));

        gfx.BindComputePipelineState(m_clearPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cb, 0, cl);
        gfx.SetComputeDescriptorTable(kUAVSlot, occupancyUav, cl);

        const uint32_t groups = (kGridDim + 7) / 8;
        gfx.DispatchCompute(groups, groups, groups, cl);
    }

    // A UAV barrier between the clear and the per-triangle voxelize dispatches
    // guarantees the clear's 0 writes are observed before the voxelize's 255
    // writes, otherwise the clear could race and wipe legitimate marks.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_occupancyTex), cl);

    // ---- Phase 2: per-mesh triangle voxelize dispatches --------------------
    if (!m_draws.empty() && m_instanceBufferSrv && m_meshDescriptorsSrv &&
        m_bindlessBufferTable)
    {
        gfx.BindComputePipelineState(m_voxelizePSO, cl);
        // UAV + scene SRVs / bindless table stay bound for all dispatches.
        gfx.SetComputeDescriptorTable(kUAVSlot,       occupancyUav,         cl);
        gfx.SetComputeDescriptorTable(kInstanceSlot,  m_instanceBufferSrv,  cl);
        gfx.SetComputeDescriptorTable(kMeshDescSlot,  m_meshDescriptorsSrv, cl);
        gfx.SetComputeDescriptorTable(kBindlessSlot,  m_bindlessBufferTable, cl);

        const DirectX::XMFLOAT3 extent {
            m_gridMax.x - m_gridMin.x,
            m_gridMax.y - m_gridMin.y,
            m_gridMax.z - m_gridMin.z
        };

        uint8_t* cbBase = static_cast<uint8_t*>(m_cbMapped);
        for (size_t i = 0; i < m_draws.size(); ++i)
        {
            const DrawEntry& d = m_draws[i];

            // Slot 0 was the clear CB; per-draw slots live at 1..kMaxDraws.
            const uint32_t slotIdx    = static_cast<uint32_t>(i + 1);
            const uint32_t byteOffset = slotIdx * kCBSlotStride;

            VoxelizeCB c{};
            c.gridMin[0] = m_gridMin.x; c.gridMin[1] = m_gridMin.y; c.gridMin[2] = m_gridMin.z;
            c.gridExtent[0] = extent.x; c.gridExtent[1] = extent.y; c.gridExtent[2] = extent.z;
            c.gridDim        = kGridDim;
            c.meshDescIdx    = d.meshDescIdx;
            c.instanceOffset = d.instanceOffset;
            c.numTriangles   = d.triangleCount;
            std::memcpy(cbBase + byteOffset, &c, sizeof(c));

            gfx.SetComputeRootCBV(kCBSlot, m_cb, byteOffset, cl);

            const uint32_t groups = (d.triangleCount + 63) / 64;
            gfx.DispatchCompute(groups, 1, 1, cl);
        }
    }

    // Hand off to LightInject.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_occupancyTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_occupancyState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    return cl;
}
